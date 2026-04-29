#pragma once
#include <QObject>
#include <QString>
#include <vector>

class WhisperBackend;

class WhisperWorker : public QObject {
  Q_OBJECT
public:
  explicit WhisperWorker(WhisperBackend* backend, QObject* parent = nullptr);

public slots:
  void loadModel(const QString& modelPath);
  void transcribe(const std::vector<float>& pcmF32);

signals:
  void modelReady();
  void modelFailed(const QString& error);
  void transcriptionReady(const QString& text);
  void transcriptionFailed(const QString& error);

private:
  WhisperBackend* m_backend;
};
