/*
 * Copyright (c) 2026 Mark Liversedge (liversedge@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#pragma once

#include "characteristic_engine.h"
#include "norm_provider.h"

#include <memory>

// The join between measured VALUES and the norms that judge them.
//
// This is what finally makes MeasureReading::hasCorridor true. The seed pack's 30 corridor signals
// have been dark since they were authored, not because nothing produced their values but because
// nothing produced their corridors — the engine correctly reported Unavailable and the whole
// library detected nothing.
//
// The split is deliberate: a VALUE source knows what the swing did and knows nothing about what is
// normal; this decorator knows what is normal and nothing about the swing. Neither has to grow the
// other's dependencies, and a test can swap either half.

namespace pinpoint::analysis {

// Values only — no corridor, no grade. Implemented by whatever can read a measure off a swing.
class IMeasureValueSource {
public:
    virtual ~IMeasureValueSource() = default;

    struct Value {
        double value      = 0.0;
        float  confidence = 1.0f;
    };

    // nullopt => this measure could not be produced for this swing. Distinct from a value of zero,
    // and the distinction is the difference between "not assessed" and "assessed and fine".
    virtual std::optional<Value> value(const QString &measureId) const = 0;
};

// Wraps a value source and attaches the norm for the shot's context.
//
// Context handling, in the three cases that matter:
//   * declared and known   — resolved up the tree from there; `contextInferred` false.
//   * NOT declared         — resolved from the default (full swing) and MARKED INFERRED, which
//                            demotes the finding's confidence in the engine. The deviation is real;
//                            what is uncertain is whether the right norm judged it.
//   * declared but unknown — resolves to NOTHING. Grading an unrecognised context against the
//                            full-swing norm would be a wrong answer wearing a right answer's
//                            clothes, so the measure reports as having no corridor and the
//                            characteristic reports Unavailable.
class NormMeasureSource final : public IMeasureSource {
public:
    NormMeasureSource(const IMeasureValueSource          &values,
                      std::shared_ptr<const INormProvider> norms,
                      QString                              contextId = QString(),
                      GradePolicy                          policy    = {})
        : m_values(values), m_norms(std::move(norms)),
          m_contextId(std::move(contextId)), m_policy(policy)
    {
    }

    std::optional<MeasureReading> read(const QString &measureId) const override
    {
        const std::optional<IMeasureValueSource::Value> v = m_values.value(measureId);
        if (!v)
            return std::nullopt;          // no value: unavailable, and never a pass

        MeasureReading r;
        r.value      = v->value;
        r.confidence = v->confidence;

        if (!m_norms)
            return r;                     // no norms wired: hasCorridor stays false, engine greys it

        const NormResolution res = m_norms->resolve(measureId, m_contextId);
        if (!res.found())
            return r;                     // no norm on the chain — same outcome, reported honestly

        r.hasCorridor     = true;
        r.greenLo         = res.norm->idealLo();
        r.greenHi         = res.norm->idealHi();
        r.grade           = grade(v->value, *res.norm, m_policy);
        r.normContextId   = res.contextId;
        r.contextInferred = m_contextId.isEmpty();
        return r;
    }

private:
    const IMeasureValueSource           &m_values;
    std::shared_ptr<const INormProvider> m_norms;
    QString                              m_contextId;
    GradePolicy                          m_policy;
};

} // namespace pinpoint::analysis
