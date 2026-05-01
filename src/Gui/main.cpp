#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>

#include "transcription_controller.h"
#include "tts_controller.h"
#include "video_controller.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);

    TranscriptionController controller;
    TtsController           ttsController;
    VideoController         videoController;

    QQmlApplicationEngine engine;
    engine.addImageProvider(QStringLiteral("videoframe"), videoController.imageProvider());
    engine.rootContext()->setContextProperty(QStringLiteral("controller"),       &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("ttsController"),    &ttsController);
    engine.rootContext()->setContextProperty(QStringLiteral("videoController"),  &videoController);

    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);

    engine.loadFromModule("PinPoint", "Main");

    return QCoreApplication::exec();
}
