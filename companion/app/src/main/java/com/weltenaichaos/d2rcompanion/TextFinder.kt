package com.weltenaichaos.d2rcompanion

import android.graphics.Bitmap
import android.graphics.Rect

/**
 * Finds D2R's text boxes from the pixels alone: bright letters on the dark
 * backdrop that item labels on the ground and item tooltips have. Monster and
 * area names have no such backdrop, so they aren't found. Only these boxes go
 * to text recognition, enlarged, instead of the whole screen.
 *
 * Steps: bright pixel runs per row (letters), joined across rows into words,
 * words with a dark gap between them into lines, then thin glyphs that the
 * word filter dropped ("T", "1") taken in at the ends.
 */
object TextFinder {

    fun find(bmp: Bitmap): List<Rect> {
        val w = bmp.width
        val h = bmp.height
        val s = h / 1080f
        val px = IntArray(w * h)
        bmp.getPixels(px, 0, w, 0, 0, w, h)
        val lum = ByteArray(w * h)
        for (i in px.indices) {
            val c = px[i]
            lum[i] = maxOf((c shr 16) and 0xFF, (c shr 8) and 0xFF, c and 0xFF).toByte()
        }
        fun l(x: Int, y: Int) = lum[y * w + x].toInt() and 0xFF

        // Bright runs per row, joined with the overlapping runs of the row above.
        val gapMax = (10 * s).toInt().coerceAtLeast(3)
        var parent = IntArray(1 shl 14)
        var sx0 = IntArray(1 shl 14); var sx1 = IntArray(1 shl 14)
        var sy = IntArray(1 shl 14); var sc = IntArray(1 shl 14)
        var n = 0
        fun root(i: Int): Int {
            var x = i
            while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x] }
            return x
        }
        fun add(y: Int, x0: Int, x1: Int, c: Int): Int {
            if (n == parent.size) {
                parent = parent.copyOf(n * 2); sx0 = sx0.copyOf(n * 2); sx1 = sx1.copyOf(n * 2)
                sy = sy.copyOf(n * 2); sc = sc.copyOf(n * 2)
            }
            parent[n] = n; sx0[n] = x0; sx1[n] = x1; sy[n] = y; sc[n] = c
            return n++
        }
        var prevStart = 0
        var prevEnd = 0
        for (y in 0 until h) {
            val rowStart = n
            var st = -1
            var last = -1
            var cnt = 0
            var x = 0
            while (x < w) {
                if (l(x, y) >= 130) {
                    if (st < 0) { st = x; cnt = 0 }
                    else if (x - last > gapMax) {
                        if (cnt >= 2) add(y, st, last, cnt)
                        st = x; cnt = 0
                    }
                    last = x; cnt++
                }
                x++
            }
            if (st >= 0 && cnt >= 2) add(y, st, last, cnt)
            for (i in rowStart until n) for (j in prevStart until prevEnd) {
                if (sx0[j] <= sx1[i] && sx1[j] >= sx0[i]) {
                    val a = root(i); val b = root(j)
                    if (a != b) parent[a] = b
                }
            }
            prevStart = rowStart
            prevEnd = n
        }

        // Bounding box per group.
        val boxes = HashMap<Int, IntArray>() // x0, y0, x1, y1, count
        for (i in 0 until n) {
            val r = root(i)
            val b = boxes[r]
            if (b == null) boxes[r] = intArrayOf(sx0[i], sy[i], sx1[i], sy[i], sc[i])
            else {
                b[0] = minOf(b[0], sx0[i]); b[1] = minOf(b[1], sy[i])
                b[2] = maxOf(b[2], sx1[i]); b[3] = maxOf(b[3], sy[i]); b[4] += sc[i]
            }
        }

        fun dark(x0: Int, x1: Int, y0: Int, y1: Int): Double {
            var sum = 0L; var cnt = 0
            for (yy in maxOf(0, y0)..minOf(h - 1, y1)) for (xx in maxOf(0, x0)..minOf(w - 1, x1)) { sum += l(xx, yy); cnt++ }
            return if (cnt == 0) 999.0 else sum.toDouble() / cnt
        }
        fun gapDark(a: Int, b: Int, y0: Int, y1: Int): Boolean {
            if (b <= a) return true
            var dk = 0; var cnt = 0; var sum = 0L
            for (yy in maxOf(0, y0)..minOf(h - 1, y1)) for (xx in maxOf(0, a) until minOf(w, b)) {
                val v = l(xx, yy); sum += v; cnt++; if (v < 60) dk++
            }
            return cnt == 0 || (dk > cnt * 0.85 && sum < 45L * cnt)
        }

        val words = ArrayList<IntArray>()
        val pieces = ArrayList<IntArray>()
        for (b in boxes.values) {
            val bw = b[2] - b[0] + 1
            val bh = b[3] - b[1] + 1
            if (bh <= 28 * s && bw <= 60 * s) pieces.add(b)
            if (bh < 7 * s || bh > 28 * s || bw < 20 * s || bw < 1.5 * bh) continue
            val dens = b[4].toDouble() / (bw * bh)
            if (dens < 0.08 || dens > 0.6) continue
            if (b[1] < 4 || b[3] + 5 >= h) continue
            // Dark backdrop just above and below the word.
            if (dark(b[0], b[2], b[1] - 4, b[1] - 2) > 70 || dark(b[0], b[2], b[3] + 2, b[3] + 4) > 70) continue
            words.add(b.copyOf(4))
        }

        // Words to lines: same band, gap between them dark (one label's backdrop).
        val lines = ArrayList<IntArray>()
        for (wd in words.sortedBy { it[0] }) {
            var merged = false
            for (ln in lines) {
                val hh = maxOf(ln[3] - ln[1], wd[3] - wd[1]) + 1
                val ov = minOf(ln[3], wd[3]) - maxOf(ln[1], wd[1])
                if (ov < 0.5 * minOf(ln[3] - ln[1], wd[3] - wd[1])) continue
                val gap = wd[0] - ln[2]
                if (gap >= -hh && gap <= 2 * hh && gapDark(ln[2] + 1, wd[0], maxOf(ln[1], wd[1]), minOf(ln[3], wd[3]))) {
                    ln[0] = minOf(ln[0], wd[0]); ln[1] = minOf(ln[1], wd[1])
                    ln[2] = maxOf(ln[2], wd[2]); ln[3] = maxOf(ln[3], wd[3])
                    merged = true
                    break
                }
            }
            if (!merged) lines.add(wd.copyOf())
        }
        // Thin glyphs at the ends.
        for (ln in lines) {
            var grew = true
            while (grew) {
                grew = false
                val hh = ln[3] - ln[1] + 1
                for (p in pieces) {
                    val cy = (p[1] + p[3]) / 2
                    if (cy < ln[1] - 2 || cy > ln[3] + 2) continue
                    if (p[0] >= ln[0] && p[2] <= ln[2]) continue
                    if (p[2] < ln[0] && ln[0] - p[2] <= 1.5 * hh && gapDark(p[2] + 1, ln[0], ln[1], ln[3])) {
                        ln[0] = p[0]; grew = true
                    } else if (p[0] > ln[2] && p[0] - ln[2] <= 1.5 * hh && gapDark(ln[2] + 1, p[0], ln[1], ln[3])) {
                        ln[2] = p[2]; grew = true
                    }
                }
            }
        }
        return lines.map { Rect(it[0], it[1], it[2] + 1, it[3] + 1) }
    }

    /**
     * The item tooltip: the biggest stack (3+) of lines centered under each
     * other with small gaps. Null when there is none.
     */
    fun tooltip(lines: List<Rect>): Rect? {
        var best: Rect? = null
        var bestCount = 0
        for (seed in lines) {
            val box = Rect(seed)
            val used = HashSet<Int>()
            used.add(lines.indexOf(seed))
            var count = 1
            var grew = true
            while (grew) {
                grew = false
                for ((i, l) in lines.withIndex()) {
                    if (i in used) continue
                    val hh = maxOf(l.height(), 8)
                    val centered = kotlin.math.abs(l.centerX() - box.centerX()) < maxOf(box.width(), l.width()) / 4 + hh
                    val near = l.top <= box.bottom + hh * 2 && l.bottom >= box.top - hh * 2
                    if (centered && near) { box.union(l); used.add(i); count++; grew = true }
                }
            }
            if (count > bestCount) { bestCount = count; best = box }
        }
        return if (bestCount >= 3) best else null
    }
}
