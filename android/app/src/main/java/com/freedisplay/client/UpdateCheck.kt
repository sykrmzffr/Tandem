package com.freedisplay.client

import java.net.HttpURLConnection
import java.net.URL
import org.json.JSONObject

/** Latest Tandem release on GitHub, read with [latest] (blocking: call off the UI thread). */
data class Release(val version: String, val apkUrl: String?, val pageUrl: String)

object UpdateCheck {
    private const val LATEST = "https://api.github.com/repos/sykrmzffr/Tandem/releases/latest"

    fun latest(): Release {
        val conn = URL(LATEST).openConnection() as HttpURLConnection
        conn.connectTimeout = 10_000
        conn.readTimeout = 15_000
        conn.setRequestProperty("Accept", "application/vnd.github+json")
        conn.setRequestProperty("User-Agent", "Tandem-Android")
        try {
            when (val code = conn.responseCode) {
                200 -> {}
                404 -> throw IllegalStateException("no release has been published yet")
                else -> throw IllegalStateException("GitHub answered $code")
            }
            val json = JSONObject(conn.inputStream.bufferedReader().use { it.readText() })
            val assets = json.optJSONArray("assets")
            var apk: String? = null
            for (i in 0 until (assets?.length() ?: 0)) {
                val url = assets!!.getJSONObject(i).optString("browser_download_url")
                if (url.endsWith("/Tandem.apk")) apk = url
            }
            return Release(json.getString("tag_name").removePrefix("v"), apk, json.optString("html_url"))
        } finally {
            conn.disconnect()
        }
    }

    /** True when [version] ("1.4.0") is newer than [current]. */
    fun isNewer(version: String, current: String): Boolean {
        val a = version.split('.').map { it.toIntOrNull() ?: 0 }
        val b = current.split('.').map { it.toIntOrNull() ?: 0 }
        for (i in 0 until maxOf(a.size, b.size)) {
            val x = a.getOrElse(i) { 0 }
            val y = b.getOrElse(i) { 0 }
            if (x != y) return x > y
        }
        return false
    }
}
