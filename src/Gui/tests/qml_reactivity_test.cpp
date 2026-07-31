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

// No binding may subscribe to a dependency it does not USE.
//
// A QML binding is subscribed to whatever properties it READS while evaluating. The idiom for
// re-running a binding that calls a Q_INVOKABLE — which cannot be bound to, having no NOTIFY — is to
// touch a revision counter inside it:
//
//     readonly property var rows: {
//         root._revision            // ← intended: "re-run me when the model changes"
//         return browser.rows(type)
//     }
//
// That line is an expression statement whose value is discarded. The QML compiler removes it, and
// the dependency goes with it: the binding evaluates once and is then subscribed to nothing. It
// keeps answering, so nothing looks broken — it just answers with the past.
//
// This shipped in the Diagnostic Model panel. Fourteen bindings had it, and the symptom that finally
// surfaced was an inspector still listing three causes after a fourth was added, while the graph
// beside it showed four because switching views re-evaluated that one for an unrelated reason. The
// gap between "wrong" and "noticed" was several sessions of work.
//
// It is invisible to review (the line is right there, and it reads as intentional — it usually has a
// comment saying so), invisible to qmllint (--confusing-expression-statement does not fire on a
// property read), and invisible to any test that loads QML through QQmlComponent::setData, because
// the interpreter keeps the read that the compiler drops. So it is checked here, in the only way
// left: by reading the source.
//
// The rule enforced is narrow and mechanical — a statement that is nothing but an identifier or a
// dotted path. Anything with a call, an operator, an assignment or a subscript is left alone.
//
//   cmake --build build/gui-tests --target qml_reactivity_test
//   ctest --test-dir build/gui-tests -R qml_reactivity --output-on-failure

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include <cstdio>

namespace {

int  g_fail = 0;
void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}

// A line that is ONLY an identifier or a dotted path: `foo`, `root._revision`, `a.b.c`.
const QRegularExpression &barePath()
{
    static const QRegularExpression re(
        QStringLiteral(R"(^\s*[A-Za-z_$][A-Za-z0-9_$]*(?:\.[A-Za-z_$][A-Za-z0-9_$]*)*\s*$)"));
    return re;
}

// The one legitimate shape that looks identical: an element of a multi-line array or argument list.
// `[\n  a,\n  b\n]` puts `b` alone on a line, and it is load-bearing there.
bool continuesAList(const QString &previous)
{
    const QString p = previous.trimmed();
    return p.endsWith(QLatin1Char(',')) || p.endsWith(QLatin1Char('['))
        || p.endsWith(QLatin1Char('(')) || p.endsWith(QLatin1Char('?'))
        || p.endsWith(QLatin1Char(':')) || p.endsWith(QLatin1Char('='))
        || p.endsWith(QStringLiteral("&&")) || p.endsWith(QStringLiteral("||"))
        || p.endsWith(QLatin1Char('+')) || p.endsWith(QLatin1Char('.'));
}

// Words that look like a bare path and are not one: control flow standing alone before its block or
// its argument on the next line.
bool isKeyword(const QString &word)
{
    static const QStringList kw{
        QStringLiteral("break"),  QStringLiteral("continue"), QStringLiteral("return"),
        QStringLiteral("else"),   QStringLiteral("do"),       QStringLiteral("try"),
        QStringLiteral("catch"),  QStringLiteral("finally"),  QStringLiteral("default"),
        QStringLiteral("case"),   QStringLiteral("true"),     QStringLiteral("false"),
        QStringLiteral("null"),   QStringLiteral("undefined"), QStringLiteral("this"),
    };
    return kw.contains(word);
}

} // namespace

int main()
{
    const QString root = QStringLiteral(PP_QML_ROOT);
    QDir          dir(root);
    if (!dir.exists()) {
        std::printf("  [FAIL] the QML tree is where the build says it is (%s)\n", qPrintable(root));
        return 1;
    }

    QStringList offenders;
    int         scanned = 0;

    QDirIterator it(root, QStringList{ QStringLiteral("*.qml") }, QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        QFile         f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        ++scanned;

        const QStringList lines = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
        QString           previous;
        bool              inBlockComment = false;

        for (int i = 0; i < lines.size(); ++i) {
            const QString raw     = lines.at(i);
            const QString trimmed = raw.trimmed();

            if (inBlockComment) {
                if (trimmed.contains(QStringLiteral("*/"))) inBlockComment = false;
                continue;
            }
            if (trimmed.startsWith(QStringLiteral("/*"))) {
                if (!trimmed.contains(QStringLiteral("*/"))) inBlockComment = true;
                continue;
            }
            if (trimmed.isEmpty() || trimmed.startsWith(QStringLiteral("//"))) continue;

            if (barePath().match(raw).hasMatch() && !continuesAList(previous)) {
                // A lone capitalised word is a type name opening a block on the next line
                // (`Rectangle` … `{`), not a statement.
                const QString head = trimmed.section(QLatin1Char('.'), 0, 0);
                const bool    isTypeName = !head.isEmpty() && head.at(0).isUpper()
                                        && !trimmed.contains(QLatin1Char('.'));
                if (!isTypeName && !isKeyword(trimmed))
                    offenders << QStringLiteral("%1:%2: %3")
                                     .arg(QDir(root).relativeFilePath(path))
                                     .arg(i + 1)
                                     .arg(trimmed);
            }
            previous = trimmed;
        }
    }

    std::printf("=== bindings may not subscribe to what they do not use ===\n");
    check(scanned > 50, "the QML tree was found and read");
    for (const QString &o : offenders) std::printf("      %s\n", qPrintable(o));
    check(offenders.isEmpty(),
          "no binding reads a dependency as a bare statement the compiler will drop");
    if (!offenders.isEmpty())
        std::printf("\n  Use the value instead of naming it — a guard is enough, and cannot be\n"
                    "  optimised away:   if (root._revision < 0) return <empty>\n");

    std::printf("\n%s (%d failure%s)\n", g_fail == 0 ? "PASS" : "FAIL", g_fail,
                g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
