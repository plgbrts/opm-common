// -*- mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
// vi: set et ts=4 sw=4 sts=4:
/*
  This file is part of the Open Porous Media project (OPM).

  OPM is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  OPM is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with OPM.  If not, see <http://www.gnu.org/licenses/>.

  Consult the COPYING file in the top-level source directory of this
  module for the precise wording of the license and the list of
  copyright holders.
*/

#include <config.h>
#include <opm/material/fluidsystems/blackoilpvt/OilPvtThermal.hpp>

#include <opm/common/ErrorMacros.hpp>

#include <opm/input/eclipse/EclipseState/EclipseState.hpp>
#include <opm/input/eclipse/EclipseState/Tables/SimpleTable.hpp>
#include <opm/input/eclipse/EclipseState/Tables/TableManager.hpp>

#include <opm/material/fluidsystems/blackoilpvt/OilPvtMultiplexer.hpp>

#include <fmt/format.h>

namespace Opm {

#if HAVE_ECL_INPUT
template<class Scalar>
void OilPvtThermal<Scalar>::
initFromState(const EclipseState& eclState, const Schedule& schedule)
{
    //////
    // initialize the isothermal part
    //////
    isothermalPvt_ = new IsothermalPvt;
    isothermalPvt_->initFromState(eclState, schedule);

    //////
    // initialize the thermal part
    //////
    const auto& tables = eclState.getTableManager();

    enableThermalDensity_ = tables.OilDenT().size() > 0;
    enableJouleThomson_ = tables.OilJT().size() > 0;
    enableThermalViscosity_ = tables.hasTables("OILVISCT");
    enableInternalEnergy_ = tables.hasTables("SPECHEAT");

    unsigned regions = isothermalPvt_->numRegions();
    setNumRegions(regions);

    // viscosity
    if (enableThermalViscosity_) {
        if (tables.getViscrefTable().empty())
            OPM_THROW(std::runtime_error, "VISCREF is required when OILVISCT is present");

        const auto& oilvisctTables = tables.getOilvisctTables();
        const auto& viscrefTable = tables.getViscrefTable();

        if (oilvisctTables.size() != regions) {
            OPM_THROW(std::runtime_error,
                      fmt::format("Tables sizes mismatch. OILVISCT: {}, NumRegions: {}\n",
                                  oilvisctTables.size(), regions));
        }
        if (viscrefTable.size() != regions) {
            OPM_THROW(std::runtime_error,
                      fmt::format("Tables sizes mismatch. VISCREF: {}, NumRegions: {}\n",
                                  viscrefTable.size(), regions));
        }

        for (unsigned regionIdx = 0; regionIdx < regions; ++regionIdx) {
            const auto& TCol = oilvisctTables[regionIdx].getColumn("Temperature").vectorCopy();
            const auto& muCol = oilvisctTables[regionIdx].getColumn("Viscosity").vectorCopy();
            oilvisctCurves_[regionIdx].setXYContainers(TCol, muCol);

            viscrefPress_[regionIdx] = viscrefTable[regionIdx].reference_pressure;
            viscrefRs_[regionIdx] = viscrefTable[regionIdx].reference_rs;

            // temperature used to calculate the reference viscosity [K]. the
            // value does not really matter if the underlying PVT object really
            // is isothermal...
            constexpr const Scalar Tref = 273.15 + 20;

            // compute the reference viscosity using the isothermal PVT object.
            viscRef_[regionIdx] =
                isothermalPvt_->viscosity(regionIdx,
                                          Tref,
                                          viscrefPress_[regionIdx],
                                          viscrefRs_[regionIdx]);
        }
    }

    // temperature dependence of oil density
    const auto& oilDenT = tables.OilDenT();
    if (oilDenT.size() > 0) {
        if (oilDenT.size() != regions) {
            OPM_THROW(std::runtime_error,
                      fmt::format("Tables sizes mismatch. OILDENT: {}, NumRegions: {}\n",
                                  oilDenT.size(), regions));
        }
        for (unsigned regionIdx = 0; regionIdx < regions; ++regionIdx) {
            const auto& record = oilDenT[regionIdx];

            oildentRefTemp_[regionIdx] = record.T0;
            oildentCT1_[regionIdx] = record.C1;
            oildentCT2_[regionIdx] = record.C2;
        }
    }

    // Joule Thomson
    if (enableJouleThomson_) {
        const auto& oilJT = tables.OilJT();
        if (oilJT.size() != regions) {
            OPM_THROW(std::runtime_error,
                      fmt::format("Tables sizes mismatch. OILJT: {}, NumRegions: {}\n",
                                  oilJT.size(), regions));
        }
        for (unsigned regionIdx = 0; regionIdx < regions; ++regionIdx) {
            const auto& record = oilJT[regionIdx];

            oilJTRefPres_[regionIdx] =  record.P0;
            oilJTC_[regionIdx] = record.C1;
        }

        const auto& densityTable = eclState.getTableManager().getDensityTable();

        if (densityTable.size() != regions) {
            OPM_THROW(std::runtime_error,
                      fmt::format("Tables sizes mismatch. DensityTable: {}, NumRegions: {}\n",
                                  densityTable.size(), regions));
        }
        for (unsigned regionIdx = 0; regionIdx < regions; ++ regionIdx) {
             rhoRefG_[regionIdx] = densityTable[regionIdx].gas;
        }
    }

    if (enableInternalEnergy_) {
        // the specific internal energy of liquid oil. be aware that ecl only specifies the
        // heat capacity (via the SPECHEAT keyword) and we need to integrate it
        // ourselfs to get the internal energy
        for (unsigned regionIdx = 0; regionIdx < regions; ++regionIdx) {
            const auto& specheatTable = tables.getSpecheatTables()[regionIdx];
            const auto& temperatureColumn = specheatTable.getColumn("TEMPERATURE");
            const auto& cvOilColumn = specheatTable.getColumn("CV_OIL");

            std::vector<double> uSamples(temperatureColumn.size());

            Scalar u = temperatureColumn[0]*cvOilColumn[0];
            for (std::size_t i = 0;; ++i) {
                uSamples[i] = u;

                if (i >= temperatureColumn.size() - 1)
                    break;

                // integrate to the heat capacity from the current sampling point to the next
                // one. this leads to a quadratic polynomial.
                Scalar c_v0 = cvOilColumn[i];
                Scalar c_v1 = cvOilColumn[i + 1];
                Scalar T0 = temperatureColumn[i];
                Scalar T1 = temperatureColumn[i + 1];
                u += 0.5*(c_v0 + c_v1)*(T1 - T0);
            }

            internalEnergyCurves_[regionIdx].setXYContainers(temperatureColumn.vectorCopy(), uSamples);
        }
    }
}
#endif

template<class Scalar>
void OilPvtThermal<Scalar>::
setNumRegions(std::size_t numRegions)
{
    oilvisctCurves_.resize(numRegions);
    viscrefPress_.resize(numRegions);
    viscrefRs_.resize(numRegions);
    viscRef_.resize(numRegions);
    internalEnergyCurves_.resize(numRegions);
    oildentRefTemp_.resize(numRegions);
    oildentCT1_.resize(numRegions);
    oildentCT2_.resize(numRegions);
    oilJTRefPres_.resize(numRegions);
    oilJTC_.resize(numRegions);
    rhoRefG_.resize(numRegions);
    hVap_.resize(numRegions, 0.0);
}

template<class Scalar>
bool OilPvtThermal<Scalar>::
operator==(const OilPvtThermal<Scalar>& data) const
{
    if (isothermalPvt_ && !data.isothermalPvt_) {
        return false;
    }
    if (!isothermalPvt_ && data.isothermalPvt_) {
        return false;
    }

    return  this->oilvisctCurves() == data.oilvisctCurves() &&
            this->viscrefPress() == data.viscrefPress() &&
            this->viscrefRs() == data.viscrefRs() &&
            this->viscRef() == data.viscRef() &&
            this->oildentRefTemp() == data.oildentRefTemp() &&
            this->oildentCT1() == data.oildentCT1() &&
            this->oildentCT2() == data.oildentCT2() &&
            this->oilJTRefPres() == data.oilJTRefPres() &&
            this->oilJTC() == data.oilJTC() &&
            this->internalEnergyCurves() == data.internalEnergyCurves() &&
            this->enableThermalDensity() == data.enableThermalDensity() &&
            this->enableJouleThomson() == data.enableJouleThomson() &&
            this->enableThermalViscosity() == data.enableThermalViscosity() &&
            this->enableInternalEnergy() == data.enableInternalEnergy();
}

template<class Scalar>
OilPvtThermal<Scalar>&
OilPvtThermal<Scalar>::
operator=(const OilPvtThermal<Scalar>& data)
{
    if (data.isothermalPvt_) {
        isothermalPvt_ = new IsothermalPvt(*data.isothermalPvt_);
    }
    else {
        isothermalPvt_ = nullptr;
    }
    oilvisctCurves_ = data.oilvisctCurves_;
    viscrefPress_ = data.viscrefPress_;
    viscrefRs_ = data.viscrefRs_;
    viscRef_ = data.viscRef_;
    oildentRefTemp_ = data.oildentRefTemp_;
    oildentCT1_ = data.oildentCT1_;
    oildentCT2_ = data.oildentCT2_;
    oilJTRefPres_ =  data.oilJTRefPres_;
    oilJTC_ =  data.oilJTC_;
    internalEnergyCurves_ = data.internalEnergyCurves_;
    enableThermalDensity_ = data.enableThermalDensity_;
    enableJouleThomson_ = data.enableJouleThomson_;
    enableThermalViscosity_ = data.enableThermalViscosity_;
    enableInternalEnergy_ = data.enableInternalEnergy_;

    return *this;
}

template class OilPvtThermal<double>;
template class OilPvtThermal<float>;

} // namespace Opm
