#pragma once
#include <QObject>
#include <QString>
#include <vector>

class STTBackend;

class STTWorker : public QObject {
  Q_OBJECT
public:
  explicit STTWorker(STTBackend* backend, QObject* parent = nullptr);

public slots:
  void loadModel(const QString& modelPath);
  void transcribe(const std::vector<float>& pcmF32);
  void stopStreaming();

signals:
  void modelReady();
  void modelFailed(const QString& error);
  void backendLabelReady(const QString& label);
  void transcriptionReady(const QString& text);
  void transcriptionFailed(const QString& error);

private:
  STTBackend* m_backend;
};
