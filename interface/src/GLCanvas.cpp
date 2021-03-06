//
//  GLCanvas.cpp
//  interface/src
//
//  Created by Stephen Birarda on 8/14/13.
//  Copyright 2013 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "Application.h"

#include "GLCanvas.h"
#include <QMimeData>
#include <QUrl>
#include <QMainWindow>

const int MSECS_PER_FRAME_WHEN_THROTTLED = 66;

GLCanvas::GLCanvas() : QGLWidget(QGLFormat(QGL::NoDepthBuffer)),
    _throttleRendering(false),
    _idleRenderInterval(MSECS_PER_FRAME_WHEN_THROTTLED)
{
}

bool GLCanvas::isThrottleRendering() const {
    return _throttleRendering || Application::getInstance()->getWindow()->isMinimized();
}

void GLCanvas::initializeGL() {
    Application::getInstance()->initializeGL();
    setAttribute(Qt::WA_AcceptTouchEvents);
    setAcceptDrops(true);
    connect(Application::getInstance(), SIGNAL(applicationStateChanged(Qt::ApplicationState)), this, SLOT(activeChanged(Qt::ApplicationState)));
    connect(&_frameTimer, SIGNAL(timeout()), this, SLOT(throttleRender()));

    // Note, we *DO NOT* want Qt to automatically swap buffers for us.  This results in the "ringing" bug mentioned in WL#19514 when we're throttling the framerate.
    setAutoBufferSwap(false);
}

void GLCanvas::paintGL() {
    if (!_throttleRendering && !Application::getInstance()->getWindow()->isMinimized()) {
        Application::getInstance()->paintGL();
        swapBuffers();
    }
}

void GLCanvas::resizeGL(int width, int height) {
    Application::getInstance()->resizeGL(width, height);
}

void GLCanvas::keyPressEvent(QKeyEvent* event) {
    Application::getInstance()->keyPressEvent(event);
}

void GLCanvas::keyReleaseEvent(QKeyEvent* event) {
    Application::getInstance()->keyReleaseEvent(event);
}

void GLCanvas::focusOutEvent(QFocusEvent* event) {
    Application::getInstance()->focusOutEvent(event);
}

void GLCanvas::mouseMoveEvent(QMouseEvent* event) {
    Application::getInstance()->mouseMoveEvent(event);
}

void GLCanvas::mousePressEvent(QMouseEvent* event) {
    Application::getInstance()->mousePressEvent(event);
}

void GLCanvas::mouseReleaseEvent(QMouseEvent* event) {
    Application::getInstance()->mouseReleaseEvent(event);
}

void GLCanvas::activeChanged(Qt::ApplicationState state) {
    switch (state) {
        case Qt::ApplicationActive:
            // If we're active, stop the frame timer and the throttle.
            _frameTimer.stop();
            _throttleRendering = false;
            break;

        case Qt::ApplicationSuspended:
        case Qt::ApplicationHidden:
            // If we're hidden or are about to suspend, don't render anything.
            _throttleRendering = false;
            _frameTimer.stop();
            break;

        default:
            // Otherwise, throttle.
            if (!_throttleRendering) {
                _frameTimer.start(_idleRenderInterval);
                _throttleRendering = true;
            }
            break;
    }
}

void GLCanvas::throttleRender() {
    _frameTimer.start(_idleRenderInterval);
    if (!Application::getInstance()->getWindow()->isMinimized()) {
        Application::getInstance()->paintGL();
        swapBuffers();
    }
}

int updateTime = 0;
bool GLCanvas::event(QEvent* event) {
    switch (event->type()) {
        case QEvent::TouchBegin:
            Application::getInstance()->touchBeginEvent(static_cast<QTouchEvent*>(event));
            event->accept();
            return true;
        case QEvent::TouchEnd:
            Application::getInstance()->touchEndEvent(static_cast<QTouchEvent*>(event));
            return true;
        case QEvent::TouchUpdate:
            Application::getInstance()->touchUpdateEvent(static_cast<QTouchEvent*>(event));
            return true;
        default:
            break;
    }
    return QGLWidget::event(event);
}

void GLCanvas::wheelEvent(QWheelEvent* event) {
    Application::getInstance()->wheelEvent(event);
}

void GLCanvas::dragEnterEvent(QDragEnterEvent* event) {
    const QMimeData *mimeData = event->mimeData();
    foreach (QUrl url, mimeData->urls()) {
        if (url.url().toLower().endsWith(SNAPSHOT_EXTENSION)) {
            event->acceptProposedAction();
            break;
        }
    }
}

void GLCanvas::dropEvent(QDropEvent* event) {
    Application::getInstance()->dropEvent(event);
}
