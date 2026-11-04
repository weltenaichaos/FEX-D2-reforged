package com.weltenaichaos.d2rcompanion

import android.Manifest
import android.app.Activity
import android.content.ContentValues
import android.content.Intent
import android.graphics.Bitmap
import android.media.projection.MediaProjectionConfig
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.Process
import android.os.SystemClock
import android.provider.MediaStore
import android.text.SpannableStringBuilder
import android.text.Spanned
import android.text.style.ForegroundColorSpan
import android.widget.Button
import android.widget.ImageView
import android.widget.Switch
import android.widget.TextView
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.Executors

class MainActivity : Activity() {

    companion object {
        var updateMessage: ((String) -> Unit)? = null
    }

    private val main = Handler(Looper.getMainLooper())
    private val worker = Executors.newSingleThreadExecutor { r ->
        Thread({
            // Keep OCR out of the game's way.
            Process.setThreadPriority(Process.THREAD_PRIORITY_BACKGROUND)
            r.run()
        }, "ocr")
    }
    private val reader by lazy { ItemReader(applicationContext) }

    private lateinit var status: TextView
    private lateinit var log: TextView
    private lateinit var shot: ImageView
    private lateinit var btnCapture: Button
    private lateinit var swAuto: Switch

    private val logText = SpannableStringBuilder()
    private var lastShot: Bitmap? = null
    private var lastLines: List<TextLine> = emptyList()
    private val seen = HashMap<String, Long>()
    @Volatile private var busy = false

    private val autoTick = object : Runnable {
        override fun run() {
            if (!swAuto.isChecked) return
            if (!busy) scan(auto = true)
            main.postDelayed(this, 2000)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        status = findViewById(R.id.status)
        log = findViewById(R.id.log)
        shot = findViewById(R.id.shot)
        btnCapture = findViewById(R.id.btnCapture)
        swAuto = findViewById(R.id.swAuto)

        if (Build.VERSION.SDK_INT >= 33) {
            requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), 2)
        }

        btnCapture.setOnClickListener {
            val svc = CaptureService.instance
            if (svc != null) svc.stopCapture() else askForCapture()
        }
        findViewById<Button>(R.id.btnRead).setOnClickListener { scan(auto = false) }
        findViewById<Button>(R.id.btnReadDelay).setOnClickListener {
            status.text = "Reading in 3 s..."
            main.postDelayed({ scan(auto = false) }, 3000)
        }
        findViewById<Button>(R.id.btnSave).setOnClickListener { saveShot() }
        swAuto.setOnCheckedChangeListener { _, on ->
            main.removeCallbacks(autoTick)
            if (on) {
                append("Watching for drops (magic, rare, set, unique, runes)\n", null)
                main.post(autoTick)
            }
        }
        CaptureService.onStopped = { main.post { updateStatus() } }
        updateMessage = { msg -> main.post { status.text = msg } }
        updateStatus()
        checkForUpdate()
    }

    /** Looks for a newer test build on GitHub and offers to install it. */
    private fun checkForUpdate() {
        val mine = packageManager.getPackageInfo(packageName, 0).longVersionCode.toInt()
        Thread {
            val rel = try { Updater.latest() } catch (e: Exception) { null }
            if (rel == null || rel.build <= mine) return@Thread
            main.post {
                if (isFinishing) return@post
                android.app.AlertDialog.Builder(this)
                    .setTitle("Test build ${rel.build} is out")
                    .setMessage("You have build $mine. Install build ${rel.build} now?")
                    .setPositiveButton("Install") { _, _ -> installUpdate(rel) }
                    .setNegativeButton("Later", null)
                    .show()
            }
        }.start()
    }

    private fun installUpdate(rel: Updater.Release) {
        if (!packageManager.canRequestPackageInstalls()) {
            status.text = "Allow D2R Companion to install apps, then reopen it"
            startActivity(
                Intent(android.provider.Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                    android.net.Uri.parse("package:$packageName"))
            )
            return
        }
        status.text = "Downloading build ${rel.build}..."
        Thread {
            try {
                Updater.install(this, rel) { pct -> main.post { status.text = "Downloading build ${rel.build}: $pct%" } }
                main.post { status.text = "Installing build ${rel.build}..." }
            } catch (e: Exception) {
                main.post { status.text = "Update failed: ${e.message}" }
            }
        }.start()
    }

    private fun askForCapture() {
        val mpm = getSystemService(MediaProjectionManager::class.java)
        // Whole screen, not one app: the game runs inside GameNative.
        val intent = if (Build.VERSION.SDK_INT >= 34) {
            mpm.createScreenCaptureIntent(MediaProjectionConfig.createConfigForDefaultDisplay())
        } else {
            mpm.createScreenCaptureIntent()
        }
        @Suppress("DEPRECATION") startActivityForResult(intent, 1)
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        @Suppress("DEPRECATION") super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != 1) return
        if (resultCode != RESULT_OK || data == null) {
            status.text = "Screen capture was not allowed"
            return
        }
        val svc = Intent(this, CaptureService::class.java)
            .putExtra(CaptureService.EXTRA_CODE, resultCode)
            .putExtra(CaptureService.EXTRA_DATA, data)
        startForegroundService(svc)
        main.postDelayed({ updateStatus() }, 500)
    }

    private fun updateStatus() {
        val svc = CaptureService.instance
        if (svc != null) {
            status.text = "Capturing game screen ${svc.width}x${svc.height} (screens ${svc.displays})"
            btnCapture.text = "Stop capture"
        } else {
            status.text = "Not capturing. Press Start capture and choose the whole screen."
            btnCapture.text = "Start capture"
            swAuto.isChecked = false
        }
    }

    private fun scan(auto: Boolean) {
        val svc = CaptureService.instance
        if (svc == null) {
            if (!auto) status.text = "Start capture first"
            return
        }
        busy = true
        worker.execute {
            try {
                val t0 = SystemClock.elapsedRealtime()
                val bmp = svc.grab()
                if (bmp == null) {
                    main.post { status.text = "No picture from the screen yet" }
                    return@execute
                }
                val t1 = SystemClock.elapsedRealtime()
                val lines = reader.read(bmp, sharp = !auto)
                val t2 = SystemClock.elapsedRealtime()
                main.post {
                    lastShot = bmp
                    lastLines = lines
                    shot.setImageBitmap(bmp)
                    status.text = "Read ${lines.size} lines (grab ${t1 - t0} ms, text ${t2 - t1} ms)"
                    if (auto) logDrops(lines) else logAll(lines)
                }
            } catch (e: Exception) {
                main.post { status.text = "Reading failed: ${e.message}" }
            } finally {
                busy = false
            }
        }
    }

    /**
     * Item labels worth logging: the text must name a known D2R item (so gold
     * monster and area names don't count) in magic, rare, set, unique or orange.
     */
    private fun isDrop(l: TextLine): Boolean = l.item != null && when (l.quality) {
        Quality.MAGIC, Quality.RARE, Quality.UNIQUE, Quality.SET, Quality.ORANGE -> l.share >= 40
        else -> false
    }

    private fun logDrops(lines: List<TextLine>) {
        val now = SystemClock.elapsedRealtime()
        seen.entries.removeAll { now - it.value > 60_000 }
        for (l in lines) {
            if (!isDrop(l) || l.text.length < 3) continue
            val key = l.text.lowercase(Locale.ROOT)
            if (seen.containsKey(key)) { seen[key] = now; continue }
            seen[key] = now
            append("${time()} DROP ", null)
            val base = l.item?.entry?.let { if (it.name != l.text) "  (${it.name})" else "" } ?: ""
            append("${l.text}$base  [${l.quality.label}]\n", l.quality.argb)
        }
    }

    private fun logAll(lines: List<TextLine>) {
        append("--- ${time()} read ---\n", null)
        // Top to bottom, then left to right, as a tooltip reads.
        for (l in lines.sortedWith(compareBy({ it.box.top / 8 }, { it.box.left }))) {
            append("${l.text}", l.quality.argb)
            val item = l.item?.entry?.let { e -> " ITEM ${e.kind.name.lowercase()}: ${e.name}" + (if (e.base != e.name) " / ${e.base}" else "") } ?: ""
            val note = item + (if (l.raw != l.text) " read as: ${l.raw}" else "") + (if (l.note.isNotEmpty()) " (${l.note})" else "")
            append("  [${l.quality.label} ${l.share}%]$note\n", 0xFF777777.toInt())
        }
    }

    private fun append(s: String, color: Int?) {
        val start = logText.length
        logText.append(s)
        if (color != null) {
            logText.setSpan(ForegroundColorSpan(color), start, logText.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
        }
        // Keep the log from growing without end.
        if (logText.length > 20000) logText.delete(0, logText.length - 15000)
        log.text = logText
    }

    private fun time() = SimpleDateFormat("HH:mm:ss", Locale.ROOT).format(Date())

    /** PNG of the last read plus the text found, in Download/D2RCompanion. */
    private fun saveShot() {
        val bmp = lastShot ?: run { status.text = "Nothing read yet"; return }
        val lines = lastLines
        worker.execute {
            try {
                val name = "d2r-shot-" + SimpleDateFormat("yyyyMMdd-HHmmss", Locale.ROOT).format(Date())
                write("$name.png", "image/png") { os -> bmp.compress(Bitmap.CompressFormat.PNG, 100, os) }
                // Same shot with the text boxes the app found, in magenta.
                val marked = bmp.copy(Bitmap.Config.ARGB_8888, true)
                val c = android.graphics.Canvas(marked)
                val p = android.graphics.Paint().apply {
                    style = android.graphics.Paint.Style.STROKE; strokeWidth = 2f; color = 0xFFFF00FF.toInt()
                }
                for (r in reader.lastBoxes) c.drawRect(r, p)
                write("$name-boxes.png", "image/png") { os -> marked.compress(Bitmap.CompressFormat.PNG, 100, os) }
                val txt = lines.joinToString("\n") {
                    "${it.text}\t${it.quality.label}\t${it.share}%\t${it.box.flattenToString()}\traw=${it.raw}\titem=${it.item?.entry?.name ?: ""}\tnote=${it.note}"
                }
                write("$name.txt", "text/plain") { os -> os.write(txt.toByteArray()) }
                main.post { status.text = "Saved Download/D2RCompanion/$name.png and .txt" }
            } catch (e: Exception) {
                main.post { status.text = "Save failed: ${e.message}" }
            }
        }
    }

    private fun write(name: String, mime: String, body: (java.io.OutputStream) -> Unit) {
        val values = ContentValues().apply {
            put(MediaStore.MediaColumns.DISPLAY_NAME, name)
            put(MediaStore.MediaColumns.MIME_TYPE, mime)
            put(MediaStore.MediaColumns.RELATIVE_PATH, "Download/D2RCompanion")
        }
        val uri = contentResolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values)
            ?: error("could not create $name")
        contentResolver.openOutputStream(uri)!!.use(body)
    }

    override fun onDestroy() {
        main.removeCallbacks(autoTick)
        CaptureService.onStopped = null
        updateMessage = null
        super.onDestroy()
    }
}
