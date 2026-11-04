package com.weltenaichaos.d2rcompanion

import android.graphics.Bitmap
import android.graphics.Rect
import com.google.android.gms.tasks.Tasks
import com.google.mlkit.vision.common.InputImage
import com.google.mlkit.vision.text.TextRecognition
import com.google.mlkit.vision.text.latin.TextRecognizerOptions

/** D2R text colors, used to tell item quality from a line of text. */
enum class Quality(val label: String, val r: Int, val g: Int, val b: Int, val argb: Int) {
    NORMAL("white", 255, 255, 255, 0xFFE8E8E8.toInt()),
    GREY("grey", 110, 110, 110, 0xFF8A8A8A.toInt()),
    MAGIC("magic", 105, 105, 255, 0xFF7878FF.toInt()),
    RARE("rare", 255, 255, 100, 0xFFFFFF64.toInt()),
    SET("set", 0, 255, 0, 0xFF00FF00.toInt()),
    UNIQUE("unique", 199, 179, 119, 0xFFC7B377.toInt()),
    ORANGE("rune/craft", 255, 168, 0, 0xFFFFA800.toInt()),
    RED("red", 255, 77, 77, 0xFFFF4D4D.toInt());
}

data class TextLine(val text: String, val box: Rect, val quality: Quality, val share: Int)

class ItemReader {
    private val recognizer = TextRecognition.getClient(TextRecognizerOptions.DEFAULT_OPTIONS)

    /** Blocking; call from a worker thread. */
    fun read(bmp: Bitmap): List<TextLine> {
        val result = Tasks.await(recognizer.process(InputImage.fromBitmap(bmp, 0)))
        val out = ArrayList<TextLine>()
        for (block in result.textBlocks) for (line in block.lines) {
            val box = line.boundingBox ?: continue
            val (q, share) = colorOf(bmp, box)
            out.add(TextLine(line.text, box, q, share))
        }
        return out
    }

    /**
     * Votes over the bright pixels inside the line's box: each picks the nearest
     * D2R text color. Returns the winner and its share of the votes in percent.
     */
    private fun colorOf(bmp: Bitmap, box: Rect): Pair<Quality, Int> {
        val r = Rect(box)
        if (!r.intersect(0, 0, bmp.width, bmp.height) || r.isEmpty) return Quality.NORMAL to 0
        val w = r.width()
        val h = r.height()
        val px = IntArray(w * h)
        bmp.getPixels(px, 0, w, r.left, r.top, w, h)
        val votes = IntArray(Quality.entries.size)
        var total = 0
        for (c in px) {
            val pr = (c shr 16) and 0xFF
            val pg = (c shr 8) and 0xFF
            val pb = c and 0xFF
            val mx = maxOf(pr, pg, pb)
            if (mx < 90) continue // background
            // Compare hue, not brightness: scale the pixel up to full brightness.
            val sr = pr * 255 / mx
            val sg = pg * 255 / mx
            val sb = pb * 255 / mx
            var best = 0
            var bestD = Int.MAX_VALUE
            for (q in Quality.entries) {
                if (q == Quality.GREY) continue
                val qm = maxOf(q.r, q.g, q.b)
                val dr = sr - q.r * 255 / qm
                val dg = sg - q.g * 255 / qm
                val db = sb - q.b * 255 / qm
                val d = dr * dr + dg * dg + db * db
                if (d < bestD) { bestD = d; best = q.ordinal }
            }
            // Dim neutral text is D2R's grey (socketed/ethereal names).
            if (best == Quality.NORMAL.ordinal && mx < 150) best = Quality.GREY.ordinal
            votes[best]++
            total++
        }
        if (total == 0) return Quality.NORMAL to 0
        val win = votes.indices.maxBy { votes[it] }
        return Quality.entries[win] to votes[win] * 100 / total
    }
}
