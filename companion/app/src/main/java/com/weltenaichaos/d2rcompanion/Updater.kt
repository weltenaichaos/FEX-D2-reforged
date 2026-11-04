package com.weltenaichaos.d2rcompanion

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.pm.PackageInstaller
import android.os.Build
import org.json.JSONArray
import java.net.HttpURLConnection
import java.net.URL

/**
 * Self-update from the GitHub releases (tags d2r-companion-<build>). Android
 * still asks once per update to confirm the install; no copying between devices.
 */
object Updater {
    private const val API =
        "https://api.github.com/repos/weltenaichaos/FEX-D2-reforged/releases?per_page=100"
    private const val PREFIX = "d2r-companion-"

    data class Release(val build: Int, val url: String, val size: Long)

    /** Newest companion release, or null. Blocking. */
    fun latest(): Release? {
        val c = URL(API).openConnection() as HttpURLConnection
        c.setRequestProperty("Accept", "application/vnd.github+json")
        c.setRequestProperty("User-Agent", "d2r-companion")
        c.connectTimeout = 10_000
        c.readTimeout = 15_000
        val body = c.inputStream.use { it.readBytes().toString(Charsets.UTF_8) }
        val arr = JSONArray(body)
        var best: Release? = null
        for (i in 0 until arr.length()) {
            val r = arr.getJSONObject(i)
            val tag = r.optString("tag_name")
            if (!tag.startsWith(PREFIX)) continue
            val build = tag.removePrefix(PREFIX).toIntOrNull() ?: continue
            val assets = r.optJSONArray("assets") ?: continue
            for (j in 0 until assets.length()) {
                val a = assets.getJSONObject(j)
                if (!a.optString("name").endsWith(".apk")) continue
                if (best == null || build > best.build) {
                    best = Release(build, a.getString("browser_download_url"), a.optLong("size", -1))
                }
            }
        }
        return best
    }

    /** Downloads the APK into an install session and hands it to Android. Blocking. */
    fun install(ctx: Context, rel: Release, progress: (Int) -> Unit) {
        val pi = ctx.packageManager.packageInstaller
        val params = PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL)
        params.setAppPackageName(ctx.packageName)
        if (Build.VERSION.SDK_INT >= 31) {
            params.setRequireUserAction(PackageInstaller.SessionParams.USER_ACTION_NOT_REQUIRED)
        }
        val id = pi.createSession(params)
        pi.openSession(id).use { session ->
            val c = URL(rel.url).openConnection() as HttpURLConnection
            c.instanceFollowRedirects = true
            c.setRequestProperty("User-Agent", "d2r-companion")
            val total = if (rel.size > 0) rel.size else c.contentLengthLong
            c.inputStream.use { input ->
                session.openWrite("app.apk", 0, total).use { out ->
                    val buf = ByteArray(256 * 1024)
                    var done = 0L
                    var lastPct = -1
                    while (true) {
                        val n = input.read(buf)
                        if (n < 0) break
                        out.write(buf, 0, n)
                        done += n
                        if (total > 0) {
                            val pct = (done * 100 / total).toInt()
                            if (pct != lastPct) { lastPct = pct; progress(pct) }
                        }
                    }
                    session.fsync(out)
                }
            }
            val intent = Intent(ctx, InstallReceiver::class.java)
            val flags = PendingIntent.FLAG_UPDATE_CURRENT or
                (if (Build.VERSION.SDK_INT >= 31) PendingIntent.FLAG_MUTABLE else 0)
            val sender = PendingIntent.getBroadcast(ctx, id, intent, flags).intentSender
            session.commit(sender)
        }
    }
}

/** Shows Android's install confirmation when it asks for one. */
class InstallReceiver : BroadcastReceiver() {
    override fun onReceive(ctx: Context, intent: Intent) {
        val status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE)
        if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
            val confirm: Intent? = if (Build.VERSION.SDK_INT >= 33) {
                intent.getParcelableExtra(Intent.EXTRA_INTENT, Intent::class.java)
            } else {
                @Suppress("DEPRECATION") intent.getParcelableExtra(Intent.EXTRA_INTENT)
            }
            confirm?.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            if (confirm != null) ctx.startActivity(confirm)
        } else if (status != PackageInstaller.STATUS_SUCCESS) {
            MainActivity.updateMessage?.invoke(
                "Update failed: " + (intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE) ?: "status $status")
            )
        }
    }
}
