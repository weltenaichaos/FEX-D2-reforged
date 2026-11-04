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

data class TextLine(
    val text: String,
    val box: Rect,
    val quality: Quality,
    val share: Int,
    /** What text recognition saw, before the D2R word fixes. */
    val raw: String = text,
    /** The known D2R item this line names, if any. */
    val item: ItemDb.Match? = null,
)

class ItemReader(ctx: android.content.Context) {
    private val db = ItemDb(ctx)
    private val words = D2RWords(ctx)
    private val recognizer = TextRecognition.getClient(TextRecognizerOptions.DEFAULT_OPTIONS)

    /**
     * Blocking; call from a worker thread. With sharp = true the largest text
     * block (the item tooltip) is cut out, scaled up 2x and read again, which
     * helps a lot with the game's lowered render resolution.
     */
    fun read(bmp: Bitmap, sharp: Boolean): List<TextLine> {
        val result = Tasks.await(recognizer.process(InputImage.fromBitmap(bmp, 0)))
        val out = ArrayList<TextLine>()
        for (block in result.textBlocks) for (line in block.lines) {
            val box = line.boundingBox ?: continue
            out.add(makeLine(line.text, bmp, box))
        }
        if (!sharp) return out

        val tip = tooltipBox(out) ?: return out
        val pad = 16
        val crop = Rect(tip.left - pad, tip.top - pad, tip.right + pad, tip.bottom + pad)
        if (!crop.intersect(0, 0, bmp.width, bmp.height)) return out
        val part = Bitmap.createBitmap(bmp, crop.left, crop.top, crop.width(), crop.height())
        val big = clean(Bitmap.createScaledBitmap(part, part.width * 2, part.height * 2, true))
        val again = Tasks.await(recognizer.process(InputImage.fromBitmap(big, 0)))
        val sharpLines = ArrayList<TextLine>()
        for (block in again.textBlocks) for (line in block.lines) {
            val b = line.boundingBox ?: continue
            val box = Rect(crop.left + b.left / 2, crop.top + b.top / 2, crop.left + b.right / 2, crop.top + b.bottom / 2)
            sharpLines.add(makeLine(line.text, bmp, box))
        }
        if (sharpLines.isEmpty()) return out
        // The 2x read wins where both found a line; a line only the first read
        // found stays (build 5 dropped lines the 2x read missed).
        out.removeAll { o -> sharpLines.any { s -> overlapsMostly(o.box, s.box) } }
        out.addAll(sharpLines)
        return out
    }

    private fun overlapsMostly(a: Rect, b: Rect): Boolean {
        val r = Rect()
        if (!r.setIntersect(a, b)) return false
        val inter = r.height().toLong()
        return inter * 2 >= minOf(a.height(), b.height())
    }

    /**
     * The item tooltip: the biggest stack of centered lines. Starts at the line
     * group with the most lines and grows by lines that are centered under it
     * and close above or below.
     */
    private fun tooltipBox(lines: List<TextLine>): Rect? {
        if (lines.isEmpty()) return null
        var bestBox: Rect? = null
        var bestCount = 0
        for (seed in lines) {
            val box = Rect(seed.box)
            var count = 1
            val used = HashSet<TextLine>()
            used.add(seed)
            var grew = true
            while (grew) {
                grew = false
                for (l in lines) {
                    if (l in used) continue
                    val h = maxOf(l.box.height(), 8)
                    val cx = l.box.centerX()
                    val centered = kotlin.math.abs(cx - box.centerX()) < maxOf(box.width(), l.box.width()) / 4 + h
                    val near = l.box.top <= box.bottom + h * 3 && l.box.bottom >= box.top - h * 3
                    if (centered && near) {
                        box.union(l.box); used.add(l); count++; grew = true
                    }
                }
            }
            if (count > bestCount) { bestCount = count; bestBox = box }
        }
        val b = bestBox ?: return null
        return if (bestCount >= 2 && b.width() >= 40) b else null
    }

    /** Dark background to black, so only the bright tooltip text is left. */
    private fun clean(bmp: Bitmap): Bitmap {
        val w = bmp.width
        val h = bmp.height
        val px = IntArray(w * h)
        bmp.getPixels(px, 0, w, 0, 0, w, h)
        for (i in px.indices) {
            val c = px[i]
            val mx = maxOf((c shr 16) and 0xFF, (c shr 8) and 0xFF, c and 0xFF)
            if (mx < 85) px[i] = 0xFF000000.toInt()
        }
        val out = bmp.copy(Bitmap.Config.ARGB_8888, true)
        out.setPixels(px, 0, w, 0, 0, w, h)
        return out
    }

    private fun makeLine(text: String, bmp: Bitmap, box: Rect): TextLine {
        val (q, share) = colorOf(bmp, box)
        val glyphs = D2RWords.fixGlyphs(text)
        // A line without numbers may be an item name: take the known name as is.
        if (glyphs.none { it.isDigit() }) {
            val m = db.match(glyphs)
            if (m != null && m.wholeLine) return TextLine(m.entry.name, box, q, share, text, m)
            if (m != null) return TextLine(D2RWords.gameCase(words.fix(glyphs)), box, q, share, text, m)
        }
        return TextLine(D2RWords.gameCase(words.fix(glyphs)), box, q, share, text)
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
