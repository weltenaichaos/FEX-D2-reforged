package com.weltenaichaos.d2rcompanion

import java.util.Locale

/**
 * Fixes text-recognition slips in D2R item text with a list of words that
 * appear on items (properties, item types, quality words). Most common first,
 * so a tie goes to the more likely word ("T" -> "to").
 */
class D2RWords(ctx: android.content.Context) {
    private val HAND = """
        to of all and the per by on in a two damage
        stash inventory personal shared difficulty normal nightmare night mare hell gold max pick up drop
        compare hold sort close menu very slow fast class encampment act
        party quest log character skill tree options potions holy
        materials gems runes shared stolen move fire cold skills skill level levels resist resistance resistances
        increased enhanced faster cast rate hit recovery attack speed run walk run/walk life mana
        defense rating chance strike attacker takes cold fire lightning poison magic physical
        reduced reduces maximum minimum adds absorb absorbs light radius find gold gold items
        better getting stamina regenerate replenish replenishes durability socketed sockets ethereal
        cannot be frozen freezes target duration slows knockback prevent monster heal heals stolen
        leech leeched leeches deadly open wounds crushing blow ignore ignores target's demons undead
        experience gained requirements required strength dexterity vitality energy character points
        based on block blocking piercing pierce sorceress amazon necromancer paladin barbarian druid
        assassin only class charges oskill aura when equipped struck kill killed you your enemy
        enemies melee ranged missile throw throwing quantity one two handed hand arrows bolts
        corpse explosion teleport fireball blizzard frozen orb nova chain static field armor
        weapon helm shield belt boots gloves ring amulet jewel charm small large grand rune key
        essence token unique set rare magic crafted superior inferior normal exceptional elite
        cracked damaged crude low quality sell value repair stack stacks tier poisonous
        rune runes runeword spell spells summon summoned minion minions fanaticism conviction
        holy shock freeze bonus potion potions healing rejuvenation scroll identify town portal
        tome book cube horadric quest jewelry staff wand scepter orb sword axe mace club hammer
        spear polearm bow crossbow dagger knife javelin claw katar mask pelt head circlet coronet
        tiara diadem plate mail leather hard studded ring chain scale breast splint field gothic
        light heavy full ancient ornate sacred war battle great long short broad crystal
        ethereal indestructible repairs self all attributes resist half freeze curse curses
        reanimate lower decrepify amplify slow attract confuse life tap iron maiden weaken
        dim vision terror warcry warcries shout battle orders command combat masteries traps
        shadow disciplines martial arts elemental shape shifting summoning offensive defensive auras
        bone necromancy curses javelin spear passive magic skills cold fire lightning spells
        target defense vs versus missiles hits ranged attacks sec seconds second
    """.trimIndent().split(Regex("\\s+")).filter { it.isNotEmpty() }

    /** Hand list first (most common words), then every word from D2R's item texts. */
    private val WORDS: List<String> = (HAND + ctx.assets.open("words.txt").bufferedReader()
        .readLines().filter { it.isNotEmpty() && !it.startsWith("#") }).distinct()

    private val SET = WORDS.toHashSet()

    /**
     * D2R's font draws O with a cross inside, which text recognition often reads
     * as a different sign (ø, ⊕, θ, Q, 0, @). Inside a word these become O again.
     */
    fun fixGlyphs(line: String): String = Companion.fixGlyphs(line)

    companion object {
    private val SMALL = setOf("to", "of", "per", "by", "on", "the", "and", "a", "in", "vs")

    /** One case style as in the game: "+110 To ATTACK" -> "+110 to Attack". */
    fun gameCase(line: String): String = Regex("\\p{L}[\\p{L}']*").replace(line) { m ->
        val w = m.value.lowercase(Locale.ROOT)
        if (w in SMALL) w else w.replaceFirstChar { it.uppercase(Locale.ROOT) }
    }

    private val NUMBER = Regex("(?<=^|[\\s+\\-:(])[|Il0-9]*[|0-9][|Il0-9]*(?=$|[\\s%:)\\-])")

    fun fixGlyphs(line: String): String {
        val sb = StringBuilder(line)
        for (i in sb.indices) {
            val c = sb[i]
            val prevLetter = i > 0 && sb[i - 1].isLetter()
            val nextLetter = i + 1 < sb.length && sb[i + 1].isLetter()
            val o = when (c) {
                'ø', 'θ', 'ο', 'о' -> 'o'
                'Ø', '⊕', '⊗', 'Θ', 'Φ', 'φ', 'Ο', 'О' -> 'O'
                // Digits and @ only when a letter is on both sides, so "40%" stays.
                '0', '@', '9' -> {
                    val nextDigit = i + 1 < sb.length && sb[i + 1].isDigit()
                    val prevDigit = i > 0 && sb[i - 1].isDigit()
                    // Inside a word, or ending one ("T9" = "to"), never in a number.
                    if (prevLetter && !nextDigit && !prevDigit) (if (sb[i - 1].isUpperCase()) 'O' else 'o') else null
                }
                '}' -> if (sb.lastIndexOf("(", i) >= 0) ')' else null
                'Q' -> if (prevLetter && nextLetter && sb[i - 1].isLowerCase()) 'o' else null
                else -> null
            }
            if (o != null) sb.setCharAt(i, o)
        }
        // "+||0" is "+110": bars and I/l inside a number are ones.
        return NUMBER.replace(sb) { m ->
            if (m.value.any { it.isDigit() } || m.range.first > 0 && sb[m.range.first - 1] == '+')
                m.value.replace('|', '1').replace('I', '1').replace('l', '1')
            else m.value
        }
    }
    }

    /** Corrects each word of a line that isn't a known word but is close to one. */
    fun fix(input: String): String {
        val line = joinSplitWords(input)
        val sb = StringBuilder()
        var last = 0
        for (m in TOKEN.findAll(line)) {
            sb.append(line, last, m.range.first)
            val prevDigit = m.range.first > 0 && line[m.range.first - 1].isDigit()
            val w = fixWord(m.value)
            if (prevDigit && w != m.value) sb.append(' ')
            sb.append(w)
            last = m.range.last + 1
        }
        sb.append(line, last, line.length)
        return sb.toString()
    }

    /**
     * Text recognition splits some words at D2R's wide letters ("D AMAGE",
     * "DA MAGE"). Two neighbours whose join is a known word become one.
     */
    private fun joinSplitWords(line: String): String {
        val parts = line.split(' ').toMutableList()
        var i = 0
        while (i < parts.size - 1) {
            val a = parts[i]
            val b = parts[i + 1]
            if (a.isNotEmpty() && b.isNotEmpty() && a.all { it.isLetter() } && b.all { it.isLetter() }) {
                val joined = (a + b).lowercase(Locale.ROOT)
                val known = joined in SET || WORDS.any {
                    it.length >= 4 && kotlin.math.abs(it.length - joined.length) <= 1 && Fuzzy.dist(joined, it, 0.5f) <= 0.4f
                }
                if (known && !(a.lowercase(Locale.ROOT) in SET && b.lowercase(Locale.ROOT) in SET)) {
                    parts[i] = a + b
                    parts.removeAt(i + 1)
                    continue
                }
            }
            i++
        }
        return parts.joinToString(" ")
    }

    private val TOKEN = Regex("[\\p{L}'@#]+")

    private fun fixWord(word: String): String {
        val lower = word.lowercase(Locale.ROOT)
        if (lower in SET) return word
        val limit = when {
            word.length <= 2 -> 0.6f
            word.length == 3 -> 1.0f
            word.length <= 6 -> 1.0f
            else -> 1.6f
        }
        var best: String? = null
        var bestD = limit + 0.001f
        for (w in WORDS) {
            if (kotlin.math.abs(w.length - lower.length) > 2) continue
            val d = Fuzzy.dist(lower, w, bestD)
            if (d < bestD) { bestD = d; best = w; if (d == 0f) break }
        }
        val b = best ?: return word
        return matchCase(word, b)
    }

    private fun matchCase(orig: String, w: String): String = when {
        orig.length > 1 && orig.all { it.isUpperCase() } -> w.uppercase(Locale.ROOT)
        orig.first().isUpperCase() && orig.length > 1 -> w.replaceFirstChar { it.uppercase(Locale.ROOT) }
        else -> w
    }
}
