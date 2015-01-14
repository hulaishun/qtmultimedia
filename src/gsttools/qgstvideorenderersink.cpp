/****************************************************************************
**
** Copyright (C) 2014 Jolla Ltd.
** Contact: http://www.qt-project.org/legal
**
** This file is part of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://qt.digia.com/licensing.  For further information
** use the contact form at http://qt.digia.com/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3.0 as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU General Public License version 3.0 requirements will be
** met: http://www.gnu.org/copyleft/gpl.html.
**
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include <qabstractvideosurface.h>
#include <qvideoframe.h>
#include <QDebug>
#include <QMap>
#include <QThread>
#include <QEvent>
#include <QCoreApplication>

#include <private/qmediapluginloader_p.h>
#include "qgstvideobuffer_p.h"

#include "qgstvideorenderersink_p.h"

#include <gst/video/video.h>

#include "qgstutils_p.h"

//#define DEBUG_VIDEO_SURFACE_SINK

QT_BEGIN_NAMESPACE

QGstDefaultVideoRenderer::QGstDefaultVideoRenderer()
    : m_flushed(true)
{
}

QGstDefaultVideoRenderer::~QGstDefaultVideoRenderer()
{
}

GstCaps *QGstDefaultVideoRenderer::getCaps(QAbstractVideoSurface *surface)
{
    return QGstUtils::capsForFormats(surface->supportedPixelFormats());
}

bool QGstDefaultVideoRenderer::start(QAbstractVideoSurface *surface, GstCaps *caps)
{
    m_flushed = true;
    m_format = QGstUtils::formatForCaps(caps, &m_videoInfo);

    return m_format.isValid() && surface->start(m_format);
}

void QGstDefaultVideoRenderer::stop(QAbstractVideoSurface *surface)
{
    m_flushed = true;
    if (surface)
        surface->stop();
}

bool QGstDefaultVideoRenderer::present(QAbstractVideoSurface *surface, GstBuffer *buffer)
{
    m_flushed = false;
    QVideoFrame frame(
                new QGstVideoBuffer(buffer, m_videoInfo),
                m_format.frameSize(),
                m_format.pixelFormat());
    QGstUtils::setFrameTimeStamps(&frame, buffer);

    return surface->present(frame);
}

void QGstDefaultVideoRenderer::flush(QAbstractVideoSurface *surface)
{
    if (surface && !m_flushed)
        surface->present(QVideoFrame());
    m_flushed = true;
}

bool QGstDefaultVideoRenderer::proposeAllocation(GstQuery *)
{
    return true;
}

Q_GLOBAL_STATIC_WITH_ARGS(QMediaPluginLoader, rendererLoader,
        (QGstVideoRendererInterface_iid, QLatin1String("video/gstvideorenderer"), Qt::CaseInsensitive))

QVideoSurfaceGstDelegate::QVideoSurfaceGstDelegate(QAbstractVideoSurface *surface)
    : m_surface(surface)
    , m_renderer(0)
    , m_activeRenderer(0)
    , m_surfaceCaps(0)
    , m_startCaps(0)
    , m_lastBuffer(0)
    , m_notified(false)
    , m_stop(false)
    , m_render(false)
    , m_flush(false)
{
    foreach (QObject *instance, rendererLoader()->instances(QGstVideoRendererPluginKey)) {
        QGstVideoRendererInterface* plugin = qobject_cast<QGstVideoRendererInterface*>(instance);
        if (QGstVideoRenderer *renderer = plugin ? plugin->createRenderer() : 0)
            m_renderers.append(renderer);
    }

    m_renderers.append(new QGstDefaultVideoRenderer);
    updateSupportedFormats();
    connect(m_surface, SIGNAL(supportedFormatsChanged()), this, SLOT(updateSupportedFormats()));
}

QVideoSurfaceGstDelegate::~QVideoSurfaceGstDelegate()
{
    qDeleteAll(m_renderers);

    if (m_surfaceCaps)
        gst_caps_unref(m_surfaceCaps);
}

GstCaps *QVideoSurfaceGstDelegate::caps()
{
    QMutexLocker locker(&m_mutex);

    gst_caps_ref(m_surfaceCaps);

    return m_surfaceCaps;
}

bool QVideoSurfaceGstDelegate::start(GstCaps *caps)
{
    QMutexLocker locker(&m_mutex);

    if (m_activeRenderer) {
        m_flush = true;
        m_stop = true;
    }

    m_render = false;

    if (m_lastBuffer) {
        gst_buffer_unref(m_lastBuffer);
        m_lastBuffer = 0;
    }

    if (m_startCaps)
        gst_caps_unref(m_startCaps);
    m_startCaps = caps;
    gst_caps_ref(m_startCaps);

    /*
    Waiting for start() to be invoked in the main thread may block
    if gstreamer blocks the main thread until this call is finished.
    This situation is rare and usually caused by setState(Null)
    while pipeline is being prerolled.

    The proper solution to this involves controlling gstreamer pipeline from
    other thread than video surface.

    Currently start() fails if wait() timed out.
    */
    if (!waitForAsyncEvent(&locker, &m_setupCondition, 1000) && m_startCaps) {
        qWarning() << "Failed to start video surface due to main thread blocked.";
        gst_caps_unref(m_startCaps);
        m_startCaps = 0;
    }

    return m_activeRenderer != 0;
}

void QVideoSurfaceGstDelegate::stop()
{
    QMutexLocker locker(&m_mutex);

    if (!m_activeRenderer)
        return;

    m_flush = true;
    m_stop = true;

    if (m_startCaps) {
        gst_caps_unref(m_startCaps);
        m_startCaps = 0;
    }

    if (m_lastBuffer) {
        gst_buffer_unref(m_lastBuffer);
        m_lastBuffer = 0;
    }

    waitForAsyncEvent(&locker, &m_setupCondition, 500);
}

bool QVideoSurfaceGstDelegate::proposeAllocation(GstQuery *query)
{
    QMutexLocker locker(&m_mutex);

    if (QGstVideoRenderer *pool = m_activeRenderer) {
        locker.unlock();

        return pool->proposeAllocation(query);
    } else {
        return false;
    }
}

void QVideoSurfaceGstDelegate::flush()
{
    QMutexLocker locker(&m_mutex);

    m_flush = true;
    m_render = false;

    if (m_lastBuffer) {
        gst_buffer_unref(m_lastBuffer);
        m_lastBuffer = 0;
    }

    notify();
}

GstFlowReturn QVideoSurfaceGstDelegate::render(GstBuffer *buffer, bool show)
{
    QMutexLocker locker(&m_mutex);

    if (m_lastBuffer)
        gst_buffer_unref(m_lastBuffer);
    m_lastBuffer = buffer;
    gst_buffer_ref(m_lastBuffer);

    if (show) {
        m_render = true;

        return waitForAsyncEvent(&locker, &m_renderCondition, 300)
                ? m_renderReturn
                : GST_FLOW_ERROR;
    } else {
        return GST_FLOW_OK;
    }
}

void QVideoSurfaceGstDelegate::handleShowPrerollChange(GObject *object, GParamSpec *, gpointer d)
{
    QVideoSurfaceGstDelegate * const delegate = static_cast<QVideoSurfaceGstDelegate *>(d);

    gboolean showPreroll = true; // "show-preroll-frame" property is true by default
    g_object_get(object, "show-preroll-frame", &showPreroll, NULL);

    GstState state = GST_STATE_NULL;
    GstState pendingState = GST_STATE_NULL;
    gst_element_get_state(GST_ELEMENT(object), &state, &pendingState, 0);

    const bool paused
                = (pendingState == GST_STATE_VOID_PENDING && state == GST_STATE_PAUSED)
                || pendingState == GST_STATE_PAUSED;

    if (paused) {
        QMutexLocker locker(&delegate->m_mutex);

        if (!showPreroll && delegate->m_lastBuffer) {
            delegate->m_render = false;
            delegate->m_flush = true;
            delegate->notify();
        } else if (delegate->m_lastBuffer) {
            delegate->m_render = true;
            delegate->notify();
        }
    }
}

bool QVideoSurfaceGstDelegate::event(QEvent *event)
{
    if (event->type() == QEvent::UpdateRequest) {
        QMutexLocker locker(&m_mutex);

        if (m_notified) {
            while (handleEvent(&locker)) {}
            m_notified = false;
        }
        return true;
    } else {
        return QObject::event(event);
    }
}

bool QVideoSurfaceGstDelegate::handleEvent(QMutexLocker *locker)
{
    if (m_flush) {
        m_flush = false;
        if (m_activeRenderer) {
            locker->unlock();

            m_activeRenderer->flush(m_surface);
        }
    } else if (m_stop) {
        m_stop = false;

        if (QGstVideoRenderer * const activePool = m_activeRenderer) {
            m_activeRenderer = 0;
            locker->unlock();

            activePool->stop(m_surface);

            locker->relock();
        }
    } else if (m_startCaps) {
        Q_ASSERT(!m_activeRenderer);

        GstCaps * const startCaps = m_startCaps;
        m_startCaps = 0;

        if (m_renderer && m_surface) {
            locker->unlock();

            const bool started = m_renderer->start(m_surface, startCaps);

            locker->relock();

            m_activeRenderer = started
                    ? m_renderer
                    : 0;
        } else if (QGstVideoRenderer * const activePool = m_activeRenderer) {
            m_activeRenderer = 0;
            locker->unlock();

            activePool->stop(m_surface);

            locker->relock();
        }

        gst_caps_unref(startCaps);
    } else if (m_render) {
        m_render = false;

        if (m_activeRenderer && m_surface && m_lastBuffer) {
            GstBuffer *buffer = m_lastBuffer;
            gst_buffer_ref(buffer);

            locker->unlock();

            const bool rendered = m_activeRenderer->present(m_surface, buffer);

            gst_buffer_unref(buffer);

            locker->relock();

            m_renderReturn = rendered
                    ? GST_FLOW_OK
                    : GST_FLOW_ERROR;

            m_renderCondition.wakeAll();
        } else {
            m_renderReturn = GST_FLOW_ERROR;
            m_renderCondition.wakeAll();
        }
    } else {
        m_setupCondition.wakeAll();

        return false;
    }
    return true;
}

void QVideoSurfaceGstDelegate::notify()
{
    if (!m_notified) {
        m_notified = true;
        QCoreApplication::postEvent(this, new QEvent(QEvent::UpdateRequest));
    }
}

bool QVideoSurfaceGstDelegate::waitForAsyncEvent(
        QMutexLocker *locker, QWaitCondition *condition, unsigned long time)
{
    if (QThread::currentThread() == thread()) {
        while (handleEvent(locker)) {}
        m_notified = false;

        return true;
    } else {
        notify();

        return condition->wait(&m_mutex, time);
    }
}

void QVideoSurfaceGstDelegate::updateSupportedFormats()
{
    if (m_surfaceCaps) {
        gst_caps_unref(m_surfaceCaps);
        m_surfaceCaps = 0;
    }

    foreach (QGstVideoRenderer *pool, m_renderers) {
        if (GstCaps *caps = pool->getCaps(m_surface)) {
            if (gst_caps_is_empty(caps)) {
                gst_caps_unref(caps);
                continue;
            }

            if (m_surfaceCaps)
                gst_caps_unref(m_surfaceCaps);

            m_renderer = pool;
            m_surfaceCaps = caps;
            break;
        } else {
            gst_caps_unref(caps);
        }
    }
}

static GstVideoSinkClass *sink_parent_class;

#define VO_SINK(s) QGstVideoRendererSink *sink(reinterpret_cast<QGstVideoRendererSink *>(s))

QGstVideoRendererSink *QGstVideoRendererSink::createSink(QAbstractVideoSurface *surface)
{
    QGstVideoRendererSink *sink = reinterpret_cast<QGstVideoRendererSink *>(
            g_object_new(QGstVideoRendererSink::get_type(), 0));

    sink->delegate = new QVideoSurfaceGstDelegate(surface);

    g_signal_connect(
                G_OBJECT(sink),
                "notify::show-preroll-frame",
                G_CALLBACK(QVideoSurfaceGstDelegate::handleShowPrerollChange),
                sink->delegate);

    return sink;
}

GType QGstVideoRendererSink::get_type()
{
    static GType type = 0;

    if (type == 0) {
        static const GTypeInfo info =
        {
            sizeof(QGstVideoRendererSinkClass),                    // class_size
            base_init,                                         // base_init
            NULL,                                              // base_finalize
            class_init,                                        // class_init
            NULL,                                              // class_finalize
            NULL,                                              // class_data
            sizeof(QGstVideoRendererSink),                         // instance_size
            0,                                                 // n_preallocs
            instance_init,                                     // instance_init
            0                                                  // value_table
        };

        type = g_type_register_static(
                GST_TYPE_VIDEO_SINK, "QGstVideoRendererSink", &info, GTypeFlags(0));
    }

    return type;
}

void QGstVideoRendererSink::class_init(gpointer g_class, gpointer class_data)
{
    Q_UNUSED(class_data);

    sink_parent_class = reinterpret_cast<GstVideoSinkClass *>(g_type_class_peek_parent(g_class));

    GstBaseSinkClass *base_sink_class = reinterpret_cast<GstBaseSinkClass *>(g_class);
    base_sink_class->get_caps = QGstVideoRendererSink::get_caps;
    base_sink_class->set_caps = QGstVideoRendererSink::set_caps;
    base_sink_class->propose_allocation = QGstVideoRendererSink::propose_allocation;
    base_sink_class->preroll = QGstVideoRendererSink::preroll;
    base_sink_class->render = QGstVideoRendererSink::render;

    GstElementClass *element_class = reinterpret_cast<GstElementClass *>(g_class);
    element_class->change_state = QGstVideoRendererSink::change_state;

    GObjectClass *object_class = reinterpret_cast<GObjectClass *>(g_class);
    object_class->finalize = QGstVideoRendererSink::finalize;
}

void QGstVideoRendererSink::base_init(gpointer g_class)
{
    static GstStaticPadTemplate sink_pad_template = GST_STATIC_PAD_TEMPLATE(
            "sink", GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS(
                    "video/x-raw, "
                    "framerate = (fraction) [ 0, MAX ], "
                    "width = (int) [ 1, MAX ], "
                    "height = (int) [ 1, MAX ]"));

    gst_element_class_add_pad_template(
            GST_ELEMENT_CLASS(g_class), gst_static_pad_template_get(&sink_pad_template));
}

void QGstVideoRendererSink::instance_init(GTypeInstance *instance, gpointer g_class)
{
    VO_SINK(instance);

    Q_UNUSED(g_class);

    sink->delegate = 0;
}

void QGstVideoRendererSink::finalize(GObject *object)
{
    VO_SINK(object);

    delete sink->delegate;

    // Chain up
    G_OBJECT_CLASS(sink_parent_class)->finalize(object);
}

GstStateChangeReturn QGstVideoRendererSink::change_state(
        GstElement *element, GstStateChange transition)
{
    Q_UNUSED(element);

    return GST_ELEMENT_CLASS(sink_parent_class)->change_state(
            element, transition);
}

GstCaps *QGstVideoRendererSink::get_caps(GstBaseSink *base, GstCaps *filter)
{
    VO_SINK(base);

    GstCaps *caps = sink->delegate->caps();
    GstCaps *unfiltered = caps;
    if (filter) {
        caps = gst_caps_intersect(unfiltered, filter);
        gst_caps_unref(unfiltered);
    }

    return caps;
}

gboolean QGstVideoRendererSink::set_caps(GstBaseSink *base, GstCaps *caps)
{
    VO_SINK(base);

#ifdef DEBUG_VIDEO_SURFACE_SINK
    qDebug() << "set_caps:";
    qDebug() << caps;
#endif

    if (!caps) {
        sink->delegate->stop();

        return TRUE;
    } else if (sink->delegate->start(caps)) {
        return TRUE;
    } else {
        return FALSE;
    }
}

gboolean QGstVideoRendererSink::propose_allocation(GstBaseSink *base, GstQuery *query)
{
    VO_SINK(base);
    return sink->delegate->proposeAllocation(query);
}

GstFlowReturn QGstVideoRendererSink::preroll(GstBaseSink *base, GstBuffer *buffer)
{
    VO_SINK(base);

    gboolean showPreroll = true; // "show-preroll-frame" property is true by default
    g_object_get(G_OBJECT(base), "show-preroll-frame", &showPreroll, NULL);

    return sink->delegate->render(buffer, showPreroll); // display frame
}

GstFlowReturn QGstVideoRendererSink::render(GstBaseSink *base, GstBuffer *buffer)
{
    VO_SINK(base);
    return sink->delegate->render(buffer, true);
}

QT_END_NAMESPACE