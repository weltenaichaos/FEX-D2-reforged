package com.weltenaichaos.d2rcompanion

import android.content.Context
import java.io.File

/**
 * Every logged drop, kept in the app's own storage (drops.tsv) so it survives
 * restarts and updates. One line per drop:
 * time (ms since 1970), session start, run, quality, text, item, raw read.
 */
class DropHistory(ctx: Context) {
    data class Drop(
        val time: Long,
        val session: Long,
        val run: Int,
        val quality: String,
        val text: String,
        val item: String,
        val raw: String,
    )

    val file = File(ctx.filesDir, "drops.tsv")
    val drops = ArrayList<Drop>()

    init {
        if (file.exists()) file.forEachLine { l ->
            val p = l.split('\t')
            if (p.size >= 7) {
                val t = p[0].toLongOrNull() ?: return@forEachLine
                drops.add(Drop(t, p[1].toLongOrNull() ?: 0, p[2].toIntOrNull() ?: 0, p[3], p[4], p[5], p[6]))
            }
        }
    }

    fun add(d: Drop) {
        drops.add(d)
        val clean = { s: String -> s.replace('\t', ' ').replace('\n', ' ') }
        file.appendText(
            "${d.time}\t${d.session}\t${d.run}\t${clean(d.quality)}\t${clean(d.text)}\t${clean(d.item)}\t${clean(d.raw)}\n"
        )
    }

    /** Removes the newest drop (for a wrong hit). */
    fun removeLast(): Drop? {
        val d = drops.removeLastOrNull() ?: return null
        val lines = file.readLines().dropLast(1)
        file.writeText(if (lines.isEmpty()) "" else lines.joinToString("\n", postfix = "\n"))
        return d
    }

    fun text(): String = if (file.exists()) file.readText() else ""
}
