package com.weltenaichaos.d2rcompanion

/**
 * Edit distance tuned to how text recognition misreads D2R's font: its O with
 * a cross is read as e, @, #, 0, θ and so on, and small capitals swap case.
 */
object Fuzzy {
    private const val O_LOOKALIKES = "e@#0θøqcaσ¤ó"

    private fun sub(a: Char, b: Char): Float {
        val x = a.lowercaseChar()
        val y = b.lowercaseChar()
        if (x == y) return 0f
        if ((y == 'o' && x in O_LOOKALIKES) || (x == 'o' && y in O_LOOKALIKES)) return 0.2f
        if (x in "il1j|!" && y in "il1j|!") return 0.3f
        if (x in "s5" && y in "s5") return 0.3f
        if (x in "z2" && y in "z2") return 0.3f
        if (x in "b8" && y in "b8") return 0.3f
        if (x in "g9" && y in "g9") return 0.4f
        if (x in "cg" && y in "cg") return 0.5f
        if (x in "dc" && y in "dc") return 0.6f
        return 1f
    }

    private fun gap(c: Char): Float = when (c.lowercaseChar()) {
        'o' -> 0.5f
        ' ', '\'', '-', '.', ',' -> 0.3f
        else -> 1f
    }

    /** Weighted distance between what was read (a) and a known text (b). */
    fun dist(a: String, b: String, limit: Float = Float.MAX_VALUE): Float {
        var prev = FloatArray(b.length + 1)
        var cur = FloatArray(b.length + 1)
        for (j in 1..b.length) prev[j] = prev[j - 1] + gap(b[j - 1])
        for (i in 1..a.length) {
            cur[0] = prev[0] + gap(a[i - 1])
            var rowMin = cur[0]
            for (j in 1..b.length) {
                val v = minOf(
                    prev[j] + gap(a[i - 1]),
                    cur[j - 1] + gap(b[j - 1]),
                    prev[j - 1] + sub(a[i - 1], b[j - 1])
                )
                cur[j] = v
                if (v < rowMin) rowMin = v
            }
            if (rowMin > limit) return rowMin
            val t = prev; prev = cur; cur = t
        }
        return prev[b.length]
    }
}
