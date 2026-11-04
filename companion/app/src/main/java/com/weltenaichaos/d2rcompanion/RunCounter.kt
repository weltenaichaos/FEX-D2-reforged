package com.weltenaichaos.d2rcompanion

import android.graphics.Bitmap

/**
 * Counts runs (games) from the screen alone.
 *
 * In game = the red life globe at the bottom left is there. A loading screen
 * is an almost black picture. A new run starts when the game comes back after
 * the globe was gone for at least [minGapMs] and a loading screen was seen in
 * between: leaving a game and making the next one takes that long, while a
 * waypoint or an act change loads in a few seconds. Open panels (inventory,
 * stash) also hide the globe but don't show a loading screen.
 */
class RunCounter {
    enum class Screen { GAME, LOADING, OTHER }

    var minGapMs = 15_000L
    @Volatile var run = 0
        private set
    @Volatile var runStart = 0L
        private set
    @Volatile var inGame = false
        private set
    @Volatile var last = Screen.OTHER
        private set
    var lastRed = 0f
        private set
    var lastDark = 0f
        private set

    private var awaySince = 0L
    private var sawLoading = false

    /** Looks at one picture; returns true when a new run started. */
    fun feed(bmp: Bitmap, now: Long): Boolean {
        val s = classify(bmp)
        last = s
        when (s) {
            Screen.GAME -> {
                val away = now - awaySince
                val newRun = run == 0 || (!inGame && sawLoading && away >= minGapMs)
                inGame = true
                sawLoading = false
                if (newRun) { run++; runStart = now; return true }
            }
            Screen.LOADING -> {
                if (inGame) awaySince = now
                inGame = false
                sawLoading = true
            }
            Screen.OTHER -> {
                if (inGame) awaySince = now
                inGame = false
            }
        }
        return false
    }

    fun bump(by: Int, now: Long) {
        run = maxOf(0, run + by)
        if (by > 0) runStart = now
    }

    fun reset() { run = 0; runStart = 0; inGame = false; sawLoading = false }

    private fun classify(bmp: Bitmap): Screen {
        val w = bmp.width
        val h = bmp.height
        // Dark share over the whole picture (every 8th pixel).
        var dark = 0; var all = 0
        val row = IntArray(w)
        var y = 0
        while (y < h) {
            bmp.getPixels(row, 0, w, 0, y, w, 1)
            var x = 0
            while (x < w) {
                val c = row[x]
                val lum = (((c shr 16) and 255) * 3 + ((c shr 8) and 255) * 6 + (c and 255)) / 10
                if (lum < 28) dark++
                all++
                x += 8
            }
            y += 8
        }
        lastDark = dark.toFloat() / all
        // Red globe: bottom left (from 1920x1080 game shots).
        val x0 = (w * 0.16f).toInt(); val x1 = (w * 0.25f).toInt()
        val y0 = (h * 0.84f).toInt(); val y1 = (h * 0.99f).toInt()
        var red = 0; var n = 0
        val part = IntArray(x1 - x0)
        y = y0
        while (y < y1) {
            bmp.getPixels(part, 0, x1 - x0, x0, y, x1 - x0, 1)
            var i = 0
            while (i < part.size) {
                val c = part[i]
                val r = (c shr 16) and 255; val g = (c shr 8) and 255; val b = c and 255
                if (r > 110 && r > 2 * g && r > 2 * b) red++
                n++
                i += 2
            }
            y += 2
        }
        lastRed = red.toFloat() / maxOf(1, n)
        return when {
            lastRed > 0.04f -> Screen.GAME
            lastDark > 0.85f -> Screen.LOADING
            else -> Screen.OTHER
        }
    }
}
