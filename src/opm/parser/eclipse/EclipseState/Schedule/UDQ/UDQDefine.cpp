/*
  Copyright 2018 Statoil ASA.

  This file is part of the Open Porous Media project (OPM).

  OPM is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  OPM is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with OPM.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <iostream>
#include <cstring>
#include <tuple>


#include <opm/parser/eclipse/Parser/ParseContext.hpp>
#include <opm/parser/eclipse/Parser/ErrorGuard.hpp>
#include <opm/parser/eclipse/EclipseState/Schedule/UDQ/UDQASTNode.hpp>
#include <opm/parser/eclipse/EclipseState/Schedule/UDQ/UDQDefine.hpp>
#include <opm/parser/eclipse/EclipseState/Schedule/UDQ/UDQEnums.hpp>

#include "../../../Parser/raw/RawConsts.hpp"
#include "UDQToken.hpp"
#include "UDQParser.hpp"

namespace Opm {



namespace {

std::vector<std::string> quote_split(const std::string& item) {
    char quote_char = '\'';
    std::vector<std::string> items;
    std::size_t offset = 0;
    while (true) {
        auto quote_pos1 = item.find(quote_char, offset);
        if (quote_pos1 == std::string::npos) {
            if (offset < item.size())
                items.push_back(item.substr(offset));
            break;
        }

        auto quote_pos2 = item.find(quote_char, quote_pos1 + 1);
        if (quote_pos2 == std::string::npos)
            throw std::invalid_argument("Unbalanced quotes in: " + item);

        if (quote_pos1 > offset)
            items.push_back(item.substr(offset, quote_pos1 - offset));
        items.push_back(item.substr(quote_pos1, 1 + quote_pos2 - quote_pos1));
        offset = quote_pos2 + 1;
    }
    return items;
}




std::vector<UDQToken> make_tokens(const std::vector<std::string>& string_tokens) {
    if (string_tokens.empty())
        return {};

    std::vector<UDQToken> tokens;
    std::size_t token_index = 0;
    while (true) {
        const auto& string_token = string_tokens[token_index];
        auto token_type = UDQ::tokenType(string_tokens[token_index]);
        token_index += 1;

        if (token_type == UDQTokenType::ecl_expr) {
            std::vector<std::string> selector;
            while (true) {
                if (token_index == string_tokens.size())
                    break;

                auto next_type = UDQ::tokenType(string_tokens[token_index]);
                if (next_type == UDQTokenType::ecl_expr) {
                    const auto& select_token = string_tokens[token_index];
                    if (RawConsts::is_quote()(select_token[0]))
                        selector.push_back(select_token.substr(1, select_token.size() - 2));
                    else
                        selector.push_back(select_token);
                    token_index += 1;
                } else
                    break;
            }
            tokens.emplace_back( string_token, selector );
        } else
            tokens.emplace_back( string_token, token_type );

        if (token_index == string_tokens.size())
            break;
    }
    return tokens;
}


}

UDQDefine::UDQDefine()
    : m_var_type(UDQVarType::NONE)
{}

template <typename T>
UDQDefine::UDQDefine(const UDQParams& udq_params_arg,
                     const std::string& keyword,
                     const std::vector<std::string>& deck_data,
                     const ParseContext& parseContext,
                     T&& errors) :
    UDQDefine(udq_params_arg, keyword, deck_data, parseContext, errors)
{}


UDQDefine::UDQDefine(const UDQParams& udq_params_arg,
                     const std::string& keyword,
                     const std::vector<std::string>& deck_data) :
    UDQDefine(udq_params_arg, keyword, deck_data, ParseContext(), ErrorGuard())
{}

namespace {
std::optional<std::string> next_token(const std::string& item, std::size_t offset, const std::vector<std::string>& splitters) {
    if (offset == item.size())
        return {};

    if (std::isdigit(item[offset])) {
        std::size_t token_size = 0;
        try {
            auto substring = item.substr(offset);
            std::ignore = std::stod(substring, &token_size);
        } catch (const std::invalid_argument &) {}

        if (token_size > 0)
            return item.substr(offset, token_size);
    }

    std::optional<std::string> token = item.substr(offset);
    std::size_t min_pos = std::string::npos;
    for (const auto& splitter : splitters) {
        auto pos = item.find(splitter, offset);
        if (pos < min_pos) {
            min_pos = pos;
            if (pos == offset)
                token = splitter;
            else
                token = item.substr(offset, pos - offset);
        }
    }
    return token;
}
} // Anonymous namespace

UDQDefine::UDQDefine(const UDQParams& udq_params,
                     const std::string& keyword,
                     const std::vector<std::string>& deck_data,
                     const ParseContext& parseContext,
                     ErrorGuard& errors) :
    m_keyword(keyword),
    m_var_type(UDQ::varType(keyword))
{
    std::vector<std::string> string_tokens;
    for (const std::string& deck_item : deck_data) {
        for (const std::string& item : quote_split(deck_item)) {
            if (RawConsts::is_quote()(item[0])) {
                string_tokens.push_back(item);
                continue;
            }

            const std::vector<std::string> splitters = {"TU*[]", "(", ")", "[", "]", ",", "+", "-", "/", "*", "==", "!=", "^", ">=", "<=", ">", "<"};
            size_t offset = 0;
            while (true) {
                auto token = next_token(item, offset, splitters);
                if (token) {
                    string_tokens.push_back( *token );
                    offset += token->size();
                } else
                    break;
            }
        }
    }
    /*
      This is hysterical special casing; the parser does not correctly handle a
      leading '-' to change sign; we just hack it up by adding a fictious '0'
      token in front.
    */
    if (string_tokens[0] == "-")
        string_tokens.insert( string_tokens.begin(), "0" );
    std::vector<UDQToken> tokens = make_tokens(string_tokens);


    this->ast = std::make_shared<UDQASTNode>( UDQParser::parse(udq_params, this->m_var_type, this->m_keyword, tokens, parseContext, errors) );
    this->string_data = "";
    for (std::size_t index = 0; index < deck_data.size(); index++) {
        this->string_data += deck_data[index];
        if (index != deck_data.size() - 1)
            this->string_data += " ";
    }
}


UDQDefine::UDQDefine(const std::string& keyword,
                     std::shared_ptr<UDQASTNode> astPtr,
                     UDQVarType type,
                     const std::string& stringData)
    : m_keyword(keyword)
    , ast(astPtr)
    , m_var_type(type)
    , string_data(stringData)
{}


UDQDefine UDQDefine::serializeObject()
{
    UDQDefine result;
    result.m_keyword = "test1";
    result.ast = std::make_shared<UDQASTNode>(UDQASTNode::serializeObject());
    result.m_var_type = UDQVarType::SEGMENT_VAR;
    result.string_data = "test2";

    return result;
}


namespace {

/*
  This function unconditinally returns true and is of-course quite useless at
  the moment; it is retained here in the hope that it is possible to actually
  make it useful in the future. See the comment in UDQEnums.hpp about 'UDQ type
  system'.
*/
bool dynamic_type_check(UDQVarType lhs, UDQVarType rhs) {
    if (lhs == rhs)
        return true;

    if (rhs == UDQVarType::SCALAR)
        return true;

    return true;
}

}

UDQSet UDQDefine::eval(UDQContext& context) const {
    UDQSet res = this->ast->eval(this->m_var_type, context);
    if (!dynamic_type_check(this->var_type(), res.var_type())) {
        std::string msg = "Invalid runtime type conversion detected when evaluating UDQ";
        throw std::invalid_argument(msg);
    }
    context.update(this->keyword(), res);

    if (res.var_type() == UDQVarType::SCALAR) {
        /*
          If the right hand side evaluates to a scalar that scalar value should
          be set for all wells in the wellset:

          UDQ
            DEFINE WUINJ1  SUM(WOPR) * 1.25 /
            DEFINE WUINJ2  WOPR OP1  * 5.0 /
          /

          Both the expressions "SUM(WOPR)" and "WOPR OP1" evaluate to a scalar,
          this should then be copied all wells, so that WUINJ1:$WELL should
          evaulate to the same numerical value for all wells. We implement the
          same behavior for group sets - but there is lots of uncertainty
          regarding the semantics of group sets.
        */

        const auto& scalar_value = res[0].value();
        if (this->var_type() == UDQVarType::WELL_VAR) {
            const std::vector<std::string> wells = context.wells();
            UDQSet well_res = UDQSet::wells(this->m_keyword, wells);

            for (const auto& well : wells)
                well_res.assign(well, scalar_value);

            return well_res;
        }

        if (this->var_type() == UDQVarType::GROUP_VAR) {
            const std::vector<std::string> groups = context.groups();
            UDQSet group_res = UDQSet::groups(this->m_keyword, groups);

            for (const auto& group : groups)
                group_res.assign(group, scalar_value);

            return group_res;
        }
    }

    res.name( this->m_keyword );
    return res;
}


UDQVarType UDQDefine::var_type() const {
    return this->m_var_type;
}


const std::string& UDQDefine::keyword() const {
    return this->m_keyword;
}

const std::string& UDQDefine::input_string() const {
    return this->string_data;
}

std::set<UDQTokenType> UDQDefine::func_tokens() const {
    return this->ast->func_tokens();
}

bool UDQDefine::operator==(const UDQDefine& data) const {
    if ((ast && !data.ast) || (!ast && data.ast))
        return false;
    if (ast && !(*ast == *data.ast))
        return false;

    return this->keyword() == data.keyword() &&
           this->var_type() == data.var_type() &&
           this->input_string() == data.input_string();
}

}
