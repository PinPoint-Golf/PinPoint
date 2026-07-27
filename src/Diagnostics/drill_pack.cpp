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

#include "drill_pack.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>

namespace pinpoint::analysis {

const Drill *DrillSet::drill(const QString &did) const
{
    const auto it = std::find_if(drills.begin(), drills.end(),
                                 [&](const Drill &d) { return d.id == did; });
    return it == drills.end() ? nullptr : &*it;
}

namespace {

void add(ValidationReport &r, IssueSeverity sev, const QString &code, const QString &subject,
         const QString &message)
{
    r.issues.push_back(ValidationIssue{ sev, code, subject, message });
}

QString userDrillPath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return dir.isEmpty() ? QString() : dir + QStringLiteral("/diagnostics/drills.json");
}

QStringList readStringList(const QJsonValue &v)
{
    QStringList out;
    for (const QJsonValue &e : v.toArray()) out << e.toString();
    return out;
}

} // namespace

ValidationReport validateDrillSet(const DrillSet &set)
{
    ValidationReport r;
    QSet<QString>    seen;

    for (const Drill &d : set.drills) {
        if (d.id.isEmpty()) {
            add(r, IssueSeverity::Error, QStringLiteral("duplicateId"), d.label,
                QStringLiteral("A drill has no id."));
            continue;
        }
        if (seen.contains(d.id))
            add(r, IssueSeverity::Error, QStringLiteral("duplicateId"), d.id,
                QStringLiteral("Duplicate drill id '%1'. Ids are permanent and must be unique.")
                    .arg(d.id));
        seen.insert(d.id);

        if (!d.id.startsWith(QStringLiteral("drill.")))
            add(r, IssueSeverity::Error, QStringLiteral("drillIdNamespace"), d.id,
                QStringLiteral("Drill id '%1' is outside the `drill.` namespace, so no "
                               "characteristic can point at it.").arg(d.id));

        if (d.instruction.trimmed().isEmpty())
            add(r, IssueSeverity::Warning, QStringLiteral("drillNoInstruction"), d.id,
                QStringLiteral("'%1' does not say what the golfer does.").arg(d.id));

        if (d.targets.trimmed().isEmpty())
            add(r, IssueSeverity::Warning, QStringLiteral("drillNoTarget"), d.id,
                QStringLiteral("'%1' does not say what it is trying to change, so nobody can judge "
                               "whether it is the right drill.").arg(d.id));
    }
    return r;
}

DrillLoadResult loadDrillSet(const QByteArray &json, const QString &sourceLabel)
{
    DrillLoadResult out;

    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        add(out.report, IssueSeverity::Error, QStringLiteral("parse"), sourceLabel,
            QStringLiteral("Could not parse the drill set '%1': %2")
                .arg(sourceLabel, pe.errorString()));
        return out;
    }

    const QJsonObject root = doc.object();
    out.set.id            = root.value(QStringLiteral("id")).toString();
    out.set.version       = root.value(QStringLiteral("version")).toString();
    out.set.schemaVersion = root.value(QStringLiteral("schemaVersion")).toInt(1);
    out.set.sourceLabel   = sourceLabel;

    for (const QJsonValue &v : root.value(QStringLiteral("drills")).toArray()) {
        const QJsonObject o = v.toObject();
        Drill             d;
        d.id          = o.value(QStringLiteral("id")).toString();
        d.label       = o.value(QStringLiteral("label")).toString();
        d.instruction = o.value(QStringLiteral("instruction")).toString();
        d.targets     = o.value(QStringLiteral("targets")).toString();
        d.equipment   = readStringList(o.value(QStringLiteral("equipment")));
        d.note        = o.value(QStringLiteral("note")).toString();
        out.set.drills.push_back(std::move(d));
    }

    out.parsed = true;
    out.report = validateDrillSet(out.set);
    out.loaded = out.report.ok();
    return out;
}

QByteArray saveDrillSet(const DrillSet &set)
{
    QJsonObject root;
    root.insert(QStringLiteral("id"), set.id);
    root.insert(QStringLiteral("version"), set.version);
    root.insert(QStringLiteral("schemaVersion"), set.schemaVersion);

    QJsonArray arr;
    for (const Drill &d : set.drills) {
        QJsonObject o;
        o.insert(QStringLiteral("id"), d.id);
        o.insert(QStringLiteral("label"), d.label);
        o.insert(QStringLiteral("instruction"), d.instruction);
        o.insert(QStringLiteral("targets"), d.targets);
        if (!d.equipment.isEmpty()) {
            QJsonArray eq;
            for (const QString &e : d.equipment) eq.append(e);
            o.insert(QStringLiteral("equipment"), eq);
        }
        if (!d.note.isEmpty()) o.insert(QStringLiteral("note"), d.note);
        arr.append(o);
    }
    root.insert(QStringLiteral("drills"), arr);

    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

namespace {

DrillSet assembleDrillSet()
{
    const QByteArray override = qgetenv("PINPOINT_CORE_DRILLS");
    const QString    corePath = override.isEmpty() ? QStringLiteral(":/diagnostics/drills.json")
                                                   : QString::fromLocal8Bit(override);

    DrillSet out;
    QFile    f(corePath);
    if (f.open(QIODevice::ReadOnly)) {
        DrillLoadResult res = loadDrillSet(f.readAll(), corePath);
        out = std::move(res.set);
    }
    out.readOnly = true;

    const QString userPath = userDrillPath();
    QFile         uf(userPath);
    if (!userPath.isEmpty() && uf.open(QIODevice::ReadOnly)) {
        DrillLoadResult res = loadDrillSet(uf.readAll(), userPath);
        if (res.parsed) {
            for (const Drill &d : res.set.drills) {
                auto it = std::find_if(out.drills.begin(), out.drills.end(),
                                       [&](const Drill &x) { return x.id == d.id; });
                if (it != out.drills.end()) *it = d;
                else                         out.drills.push_back(d);
            }
            out.readOnly = false;
        }
    }
    return out;
}

DrillSet *g_drills = nullptr;

} // namespace

const DrillSet &sharedDrillSet()
{
    if (!g_drills) g_drills = new DrillSet(assembleDrillSet());
    return *g_drills;
}

void resetSharedDrillSet()
{
    delete g_drills;
    g_drills = nullptr;
}

} // namespace pinpoint::analysis
