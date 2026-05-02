#include "video_input_base.h"

VideoInputBase::VideoInputBase(QObject *parent)
    : QObject(parent)
{
}

VideoInputBase::State VideoInputBase::state() const
{
    return m_state;
}
