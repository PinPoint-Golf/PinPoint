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

#include <QString>
#include <QStringList>

#include <cstddef>

// The (enum → token, label) table machinery the Diagnostics vocabularies are spelled with. One table
// per enum, so a spelling exists in exactly one place; every `name` is a stable JSON token and must
// never change once a pack or a norm set has shipped with it.
//
// It lives in a header because it had lived in two, and they had already begun to disagree.
// characteristic.cpp and norm_pack.cpp each carried a private copy — byte-identical but for one
// line, which is the worst possible amount of difference. The norm side had been taught that a
// LABEL is decoded with fromUtf8 (its age bands carry an en dash: "55–64"); the characteristic side
// still read labels as Latin-1. Nothing was broken, because no characteristic label had yet used a
// character outside ASCII — the divergence was not a bug but a trap primed for whoever wrote the
// first one, and it would have sprung silently: mojibake on screen, nothing failing anywhere to say
// so. Copies drift towards the version that was last thought about, and only one of two copies ever
// gets the thought. So: one copy, on the side of the argument that had been made.
//
// This is `_p` — internal to src/Diagnostics, included by the .cpp files that own the tables. It is
// deliberately NOT part of any public vocabulary header: a caller outside gets the named accessors
// (`gradeLabel()`, `shapeName()`, …), never the table.

namespace pinpoint::analysis::detail {

template <typename E>
struct Row {
    E           value;
    const char *name;
    const char *label;
};

template <typename E, size_t N>
QString nameOf(const Row<E> (&rows)[N], E v)
{
    for (const auto &r : rows)
        if (r.value == v) return QString::fromLatin1(r.name);
    return QString::fromLatin1(rows[0].name);
}

// fromUtf8 on the LABEL and fromLatin1 on the token, and the asymmetry is not an oversight. A token
// is ASCII by contract — it is a JSON key that must never change. A label is prose, and the age-band
// labels carry an en dash ("18–54"); read as Latin-1 that becomes three mojibake characters on
// screen, with nothing failing anywhere to say so.
template <typename E, size_t N>
QString labelOf(const Row<E> (&rows)[N], E v)
{
    for (const auto &r : rows)
        if (r.value == v) return QString::fromUtf8(r.label);
    return QString::fromUtf8(rows[0].label);
}

template <typename E, size_t N>
bool fromName(const Row<E> (&rows)[N], const QString &s, E &out)
{
    for (const auto &r : rows)
        if (s == QLatin1String(r.name)) { out = r.value; return true; }
    return false;
}

// The tokens of a table as one comma-separated string — for the loader diagnostics that have to tell
// an author what the closed vocabulary actually is. Tokens, not labels, because the message names
// what may be TYPED into the file.
template <typename E, size_t N>
QString tokenList(const Row<E> (&rows)[N])
{
    QStringList out;
    for (const auto &r : rows) out << QString::fromLatin1(r.name);
    return out.join(QStringLiteral(", "));
}

} // namespace pinpoint::analysis::detail
