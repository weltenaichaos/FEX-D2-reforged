package com.weltenaichaos.d2rcompanion

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.graphics.Bitmap
import android.graphics.PixelFormat
import android.graphics.Point
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.media.ImageReader
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.IBinder
import android.os.SystemClock
import android.view.Display

/**
 * Mirrors the main display (the Thor's top screen, where the game runs) into an
 * ImageReader, but only while grab() runs: between reads the virtual display has
 * no surface, so the compositor does no extra work for it and the game's GPU and
 * memory are left alone (build 1 mirrored all the time, and the game crashed in
 * loading screens). It only sees pixels, like a screenshot.
 */
class CaptureService : Service() {

    companion object {
        const val EXTRA_CODE = "code"
        const val EXTRA_DATA = "data"
        @Volatile var instance: CaptureService? = null
            private set
        var onStopped: (() -> Unit)? = null
    }

    private var projection: MediaProjection? = null
    private var virtualDisplay: VirtualDisplay? = null
    private var reader: ImageReader? = null
    private var thread: HandlerThread? = null

    var width = 0
        private set
    var height = 0
        private set

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        startAsForeground()

        val code = intent?.getIntExtra(EXTRA_CODE, 0) ?: 0
        val data: Intent? = if (Build.VERSION.SDK_INT >= 33) {
            intent?.getParcelableExtra(EXTRA_DATA, Intent::class.java)
        } else {
            @Suppress("DEPRECATION") intent?.getParcelableExtra(EXTRA_DATA)
        }
        if (data == null || projection != null) {
            if (projection == null) stopSelf()
            return START_NOT_STICKY
        }

        val mpm = getSystemService(MediaProjectionManager::class.java)
        val p = mpm.getMediaProjection(code, data)
        if (p == null) {
            stopSelf()
            return START_NOT_STICKY
        }
        projection = p

        val t = HandlerThread("capture").apply { start() }
        thread = t
        val handler = Handler(t.looper)
        p.registerCallback(object : MediaProjection.Callback() {
            override fun onStop() = stopCapture()
        }, handler)

        val display = getSystemService(DisplayManager::class.java).getDisplay(Display.DEFAULT_DISPLAY)
        val size = Point()
        @Suppress("DEPRECATION") display.getRealSize(size)
        width = size.x
        height = size.y

        val r = ImageReader.newInstance(width, height, PixelFormat.RGBA_8888, 2)
        reader = r
        virtualDisplay = p.createVirtualDisplay(
            "d2r-companion", width, height, resources.displayMetrics.densityDpi,
            DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR, null, null, handler
        )
        instance = this
        return START_NOT_STICKY
    }

    /** One fresh frame of the game screen, or null if none came in time. */
    @Synchronized
    fun grab(): Bitmap? {
        val r = reader ?: return null
        val vd = virtualDisplay ?: return null
        // Drop a frame left over from the last read.
        try { r.acquireLatestImage()?.close() } catch (e: Exception) {}
        vd.surface = r.surface
        try {
            val until = SystemClock.uptimeMillis() + 1500
            SystemClock.sleep(40)
            while (true) {
                val image = try { r.acquireLatestImage() } catch (e: Exception) { null }
                if (image != null) {
                    try {
                        val plane = image.planes[0]
                        val rowPixels = plane.rowStride / plane.pixelStride
                        val full = Bitmap.createBitmap(rowPixels, image.height, Bitmap.Config.ARGB_8888)
                        full.copyPixelsFromBuffer(plane.buffer)
                        if (rowPixels == image.width) return full
                        val bmp = Bitmap.createBitmap(full, 0, 0, image.width, image.height)
                        full.recycle()
                        return bmp
                    } finally {
                        image.close()
                    }
                }
                if (SystemClock.uptimeMillis() > until) return null
                SystemClock.sleep(30)
            }
        } finally {
            vd.surface = null
        }
    }

    private fun startAsForeground() {
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(
            NotificationChannel("capture", "Screen reading", NotificationManager.IMPORTANCE_LOW)
        )
        val n = Notification.Builder(this, "capture")
            .setContentTitle("D2R Companion")
            .setContentText("Reading the game screen")
            .setSmallIcon(android.R.drawable.ic_menu_search)
            .setOngoing(true)
            .build()
        if (Build.VERSION.SDK_INT >= 29) {
            startForeground(1, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION)
        } else {
            startForeground(1, n)
        }
    }

    fun stopCapture() {
        virtualDisplay?.release(); virtualDisplay = null
        reader?.close(); reader = null
        projection?.let { p -> projection = null; p.stop() }
        thread?.quitSafely(); thread = null
        instance = null
        onStopped?.invoke()
        stopForeground(STOP_FOREGROUND_REMOVE)
        stopSelf()
    }

    override fun onDestroy() {
        if (projection != null) stopCapture()
        super.onDestroy()
    }
}
