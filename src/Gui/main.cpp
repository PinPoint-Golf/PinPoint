#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>

#include "transcription_controller.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);

    TranscriptionController controller;

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("controller"), &controller);

    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);

    engine.loadFromModule("PinPoint", "Main");

    return QCoreApplication::exec();
}
