package com.weltenaichaos.d2rcompanion

import android.graphics.Bitmap
import android.graphics.Rect
import com.google.android.gms.tasks.Tasks
import com.google.mlkit.vision.common.InputImage
import com.google.mlkit.vision.text.TextRecognition
import com.google.mlkit.vision.text.latin.TextRecognizerOptions
import java.util.Locale

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
    /** How the line was corrected beyond plain reading, for the log. */
    val note: String = "",
)

class ItemReader(ctx: android.content.Context) {
    private val db = ItemDb(ctx)
    private val words = D2RWords(ctx)
    private val recognizer = TextRecognition.getClient(TextRecognizerOptions.DEFAULT_OPTIONS)

    /** Text boxes the last read found on the screen (for the saved shot). */
    @Volatile var lastBoxes: List<Rect> = emptyList()
        private set

    /**
     * Blocking; call from a worker thread. First finds D2R's text boxes from
     * the pixels (TextFinder). sharp = true (Read now): the item tooltip is cut
     * out, enlarged 2x and read; without a tooltip the whole screen is read as
     * before. sharp = false (watch): only the label boxes are read, enlarged and
     * put together into one picture, so one cheap read covers all of them.
     */
    fun read(bmp: Bitmap, sharp: Boolean): List<TextLine> {
        val found = TextFinder.find(bmp)
        lastBoxes = found
        val tip = TextFinder.tooltip(found)
        if (!sharp) return readLabels(bmp, found.filter { tip == null || !Rect.intersects(it, tip) })
        if (tip != null) {
            val pad = maxOf(12, bmp.height / 90)
            val crop = Rect(tip.left - pad, tip.top - pad, tip.right + pad, tip.bottom + pad)
            if (crop.intersect(0, 0, bmp.width, bmp.height)) {
                val out = readRegion(bmp, crop)
                if (out.isNotEmpty()) return finishTooltip(bmp, out, crop)
            }
        }
        return readFull(bmp)
    }

    /** Text in one part of the screen, enlarged 2x with the backdrop blacked out. */
    private fun readRegion(bmp: Bitmap, crop: Rect): MutableList<TextLine> {
        val part = Bitmap.createBitmap(bmp, crop.left, crop.top, crop.width(), crop.height())
        val big = clean(Bitmap.createScaledBitmap(part, part.width * 2, part.height * 2, true))
        val res = Tasks.await(recognizer.process(InputImage.fromBitmap(big, 0)))
        val out = ArrayList<TextLine>()
        for (block in res.textBlocks) for (line in block.lines) {
            val b = line.boundingBox ?: continue
            val box = Rect(crop.left + b.left / 2, crop.top + b.top / 2, crop.left + b.right / 2, crop.top + b.bottom / 2)
            out.add(makeLine(line.text, bmp, box))
        }
        return out
    }

    /** Build 8's way: the whole screen, then the tooltip again at 2x. */
    private fun readFull(bmp: Bitmap): List<TextLine> {
        val result = Tasks.await(recognizer.process(InputImage.fromBitmap(bmp, 0)))
        val out = ArrayList<TextLine>()
        for (block in result.textBlocks) for (line in block.lines) {
            val box = line.boundingBox ?: continue
            out.add(makeLine(line.text, bmp, box))
        }
        val tip = tooltipBox(out) ?: return out
        val pad = 16
        val crop = Rect(tip.left - pad, tip.top - pad, tip.right + pad, tip.bottom + pad)
        if (!crop.intersect(0, 0, bmp.width, bmp.height)) return out
        val sharpLines = readRegion(bmp, crop)
        if (sharpLines.isEmpty()) return out
        // The 2x read wins where both found a line; a line only the first read found stays.
        out.removeAll { o -> sharpLines.any { s -> overlapsMostly(o.box, s.box) } }
        out.addAll(sharpLines)
        return finishTooltip(bmp, out, crop)
    }

    private fun finishTooltip(bmp: Bitmap, out: MutableList<TextLine>, crop: Rect): List<TextLine> {
        // Lines whose numbers came out broken get a second read on their own, 4x.
        for (i in out.indices) {
            val l = out[i]
            if (!Rect.intersects(l.box, crop) || !numbersBroken(l.text)) continue
            val again2 = rereadLine(bmp, l.box) ?: continue
            val nl = makeLine(again2, bmp, l.box)
            if (!numbersBroken(nl.text)) out[i] = nl.copy(raw = l.raw, note = "re-read 4x: $again2")
        }
        workOutDamage(out, crop)
        return out
    }

    /** All label boxes in one picture, each enlarged 2x, read in one go. */
    private fun readLabels(bmp: Bitmap, boxes: List<Rect>): List<TextLine> {
        val crops = ArrayList<Rect>()
        for (b in boxes) {
            val p = maxOf(3, b.height() / 3)
            val r = Rect(b.left - p, b.top - p, b.right + p, b.bottom + p)
            if (r.intersect(0, 0, bmp.width, bmp.height)) crops.add(r)
            if (crops.size >= 60) break
        }
        if (crops.isEmpty()) return emptyList()
        val gap = 16
        val mw = crops.maxOf { it.width() } * 2
        val tops = IntArray(crops.size)
        var mh = 0
        for ((i, c) in crops.withIndex()) { tops[i] = mh; mh += c.height() * 2 + gap }
        val mosaic = Bitmap.createBitmap(mw, mh, Bitmap.Config.ARGB_8888)
        val canvas = android.graphics.Canvas(mosaic)
        canvas.drawColor(android.graphics.Color.BLACK)
        val paint = android.graphics.Paint(android.graphics.Paint.FILTER_BITMAP_FLAG)
        for ((i, c) in crops.withIndex()) {
            canvas.drawBitmap(bmp, c, Rect(0, tops[i], c.width() * 2, tops[i] + c.height() * 2), paint)
        }
        val res = Tasks.await(recognizer.process(InputImage.fromBitmap(clean(mosaic), 0)))
        val out = ArrayList<TextLine>()
        for (block in res.textBlocks) for (line in block.lines) {
            val b = line.boundingBox ?: continue
            val cy = b.centerY()
            var i = tops.indexOfLast { it <= cy }
            if (i < 0) i = 0
            val c = crops[i]
            val box = Rect(c.left + b.left / 2, c.top + (b.top - tops[i]) / 2, c.left + b.right / 2, c.top + (b.bottom - tops[i]) / 2)
            box.intersect(c)
            out.add(makeLine(line.text, bmp, box))
        }
        return out
    }

    private fun numbersBroken(t: String): Boolean {
        val l = t.lowercase(Locale.ROOT)
        if ('|' in t) return true
        if ("damage:" in l && !Regex("damage: \\d+ to \\d+").containsMatchIn(l)) return true
        if ("durability:" in l && !Regex("durability: \\d+ of \\d+").containsMatchIn(l)) return true
        if ((l.startsWith("required") || l.startsWith("defense:")) && !Regex(": \\d+\\s*$").containsMatchIn(l)) return true
        return false
    }

    private fun rereadLine(bmp: Bitmap, box: Rect): String? {
        val pad = maxOf(4, box.height() / 3)
        val r = Rect(box.left - pad * 2, box.top - pad, box.right + pad * 2, box.bottom + pad)
        if (!r.intersect(0, 0, bmp.width, bmp.height)) return null
        val part = Bitmap.createBitmap(bmp, r.left, r.top, r.width(), r.height())
        val big = clean(Bitmap.createScaledBitmap(part, part.width * 4, part.height * 4, true))
        val res = Tasks.await(recognizer.process(InputImage.fromBitmap(big, 0)))
        val lines = res.textBlocks.flatMap { it.lines }.sortedBy { it.boundingBox?.left ?: 0 }
        if (lines.isEmpty()) return null
        return lines.joinToString(" ") { it.text }
    }

    /**
     * Weapon damage follows from the base item and enhanced damage:
     * Giant Sword 9-28 with +86% = 16 to 52. A line read with numbers missing
     * gets them worked out; one that matches is confirmed.
     */
    private fun workOutDamage(lines: MutableList<TextLine>, tip: Rect) {
        val tl = lines.filter { Rect.intersects(it.box, tip) }.sortedBy { it.box.top }
        var base: ItemDb.Entry? = null
        for (l in tl) {
            val e = l.item?.entry ?: continue
            val b = if (e.kind == ItemDb.Kind.BASE) e else db.base(e.base)
            if (b != null && b.damage.isNotEmpty()) { base = b; break }
        }
        val b = base ?: return
        val all = tl.joinToString("\n") { it.text.lowercase(Locale.ROOT) }
        // Flat damage bonuses change the numbers in ways we don't model.
        if (Regex("to (maximum|minimum) damage|adds \\d+-\\d+ damage|damage \\(based on").containsMatchIn(all)) return
        val ed = Regex("\\+(\\d+)% enhanced damage").find(all)?.groupValues?.get(1)?.toInt() ?: 0
        val eth = "ethereal" in all
        for (i in lines.indices) {
            val l = lines[i]
            if (!Rect.intersects(l.box, tip)) continue
            val lo = l.text.lowercase(Locale.ROOT)
            val (tag, label) = when {
                lo.startsWith("one-hand damage") -> "1h" to "One-Hand"
                lo.startsWith("two-hand damage") -> "2h" to "Two-Hand"
                lo.startsWith("throw damage") -> "th" to "Throw"
                else -> continue
            }
            val r = b.damage[tag] ?: continue
            fun calc(v: Int): Int { val x = if (eth) v * 3 / 2 else v; return x * (100 + ed) / 100 }
            val mn = calc(r.first)
            val mx = calc(r.last)
            val m = Regex("damage: (\\d+) to (\\d+)").find(lo)
            val a = m?.groupValues?.get(1)?.toIntOrNull()
            val z = m?.groupValues?.get(2)?.toIntOrNull()
            val how = "${b.name} ${r.first}-${r.last}" + (if (ed > 0) " +$ed%" else "") + (if (eth) " ethereal" else "")
            when {
                a == mn && z == mx -> lines[i] = l.copy(note = (l.note + " checked: $how").trim())
                m == null || a == mn || z == mx ->
                    lines[i] = l.copy(text = "$label Damage: $mn to $mx", note = (l.note + " worked out: $how").trim())
            }
        }
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
        // (A digit inside a word is a misread letter, "PT1@N", not a number.)
        if (!Regex("(?<!\\p{L})\\d+(?!\\p{L})").containsMatchIn(glyphs)) {
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
