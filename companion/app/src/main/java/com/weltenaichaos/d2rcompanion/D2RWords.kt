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
        compare hold sort close menu very slow fast class encampment act skills skill level levels resist resistance resistances
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
                '0', '@' -> if (prevLetter && nextLetter) (if (sb[i - 1].isUpperCase()) 'O' else 'o') else null
                'Q' -> if (prevLetter && nextLetter && sb[i - 1].isLowerCase()) 'o' else null
                else -> null
            }
            if (o != null) sb.setCharAt(i, o)
        }
        return sb.toString()
    }
    }

    /** Corrects each word of a line that isn't a known word but is close to one. */
    fun fix(line: String): String {
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

    private val TOKEN = Regex("[A-Za-z'@#θøØ]+")

    private fun fixWord(word: String): String {
        val lower = word.lowercase(Locale.ROOT)
        if (lower in SET) return word
        val limit = when {
            word.length <= 3 -> 0.6f
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
