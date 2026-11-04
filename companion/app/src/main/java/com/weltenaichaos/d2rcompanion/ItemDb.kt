package com.weltenaichaos.d2rcompanion

import android.content.Context
import java.util.Locale

/**
 * Every D2R item name (bases incl. runes, gems, potions; uniques; set items)
 * from assets/items.tsv. A line of text only counts as an item when it matches
 * one, which keeps monster and area names (also gold-tinted) out of the log.
 */
class ItemDb(ctx: Context) {
    enum class Kind { BASE, UNIQUE, SET }
    data class Entry(val kind: Kind, val name: String, val base: String, val key: String)
    data class Match(val entry: Entry, val score: Float, val wholeLine: Boolean)

    private val entries: List<Entry>
    private val bases: List<Entry>

    init {
        val list = ArrayList<Entry>()
        ctx.assets.open("items.tsv").bufferedReader().useLines { lines ->
            for (l in lines) {
                if (l.startsWith("#")) continue
                val p = l.split('\t')
                if (p.size < 3) continue
                val kind = when (p[0]) { "unique" -> Kind.UNIQUE; "set" -> Kind.SET; else -> Kind.BASE }
                list.add(Entry(kind, p[1], p[2], p[1].lowercase(Locale.ROOT)))
            }
        }
        entries = list
        bases = list.filter { it.kind == Kind.BASE }
    }

    /**
     * Best item for a line of text: the whole line as a known name, or else a
     * known base name inside it (magic and rare names wrap the base in affixes).
     * score = weighted misreads per letter, 0 = exact.
     */
    fun match(line: String, maxScore: Float = 0.22f): Match? {
        val text = line.lowercase(Locale.ROOT).trim()
        if (text.length < 3 || text.count { it.isLetter() } < 3) return null
        var best: Match? = null
        for (e in entries) {
            if (kotlin.math.abs(e.key.length - text.length) > e.key.length * maxScore + 2) continue
            val limit = e.key.length * maxScore
            val d = Fuzzy.dist(text, e.key, limit)
            val s = d / e.key.length
            if (s <= maxScore && (best == null || s < best.score)) best = Match(e, s, true)
        }
        if (best != null) return best

        // Tooltip lines like "Spear Class - Very Slow Attack Speed" aren't names.
        if (':' in text || " - " in text) return null
        val words = text.split(' ').filter { it.isNotEmpty() }
        for (e in bases) {
            val n = e.key.count { it == ' ' } + 1
            if (n >= words.size || e.key.length < 4) continue
            for (i in 0..words.size - n) {
                val window = words.subList(i, i + n).joinToString(" ")
                // Stricter inside a longer line, so "Class" doesn't become "Claws".
                val inner = maxScore * 0.5f
                val d = Fuzzy.dist(window, e.key, e.key.length * inner)
                val s = d / e.key.length
                if (s <= inner && (best == null || s < best.score)) best = Match(e, s, false)
            }
        }
        return best
    }
}
