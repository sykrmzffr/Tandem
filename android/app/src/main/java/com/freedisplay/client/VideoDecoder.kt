package com.freedisplay.client

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaFormat
import android.os.Build
import android.util.Log
import android.view.Surface

/**
 * Hardware H.264 decoder rendering straight to a Surface with minimal buffering.
 * [onDecoded] receives (ptsUs, µs from queueing to decoded output) for each frame.
 */
class VideoDecoder(
    private val surface: Surface,
    private val rotationDegrees: Int, // clockwise, applied on screen (0, 90 or 270)
    private val onStarted: (String) -> Unit,
    private val onFailed: (String) -> Unit,
    private val onDecoded: (ptsUs: Long, decodeUs: Int, inputWaitUs: Int) -> Unit,
) {
    /** pts -> (nanoTime the frame finished arriving, nanoTime it was queued to the codec) */
    private val queuedAt = java.util.concurrent.ConcurrentHashMap<Long, Pair<Long, Long>>()
    private var codec: MediaCodec? = null
    private var outputThread: Thread? = null
    @Volatile private var running = false
    private var width = 0
    private var height = 0
    private var retryAtNs = 0L // after a failed start, wait before trying again
    private var lastFailure = ""

    fun configure(w: Int, h: Int) {
        release()
        width = w
        height = h
    }

    /** Returns false when the decoder needs a keyframe to (re)start. */
    fun feed(data: ByteArray, offset: Int, length: Int, ptsUs: Long, keyframe: Boolean,
             receivedNs: Long = System.nanoTime()): Boolean {
        var c = codec
        if (c == null) {
            // Retrying on every keyframe kept this thread busy for hundreds of ms per frame
            // (pings went unanswered); drop frames for a while instead.
            if (System.nanoTime() < retryAtNs) return true
            if (!keyframe || width == 0) return false
            c = start(data, offset, length)
            if (c == null) {
                retryAtNs = System.nanoTime() + 3_000_000_000L
                return true
            }
        }
        return try {
            val index = c.dequeueInputBuffer(50_000)
            if (index < 0) return true // decoder busy; drop this frame
            val buf = c.getInputBuffer(index) ?: return true
            buf.clear()
            buf.put(data, offset, length)
            if (queuedAt.size > 300) queuedAt.clear()
            queuedAt[ptsUs] = Pair(receivedNs, System.nanoTime())
            c.queueInputBuffer(index, 0, length, ptsUs, 0)
            true
        } catch (e: Exception) {
            Log.w(TAG, "decode failed", e)
            release()
            false
        }
    }

    private fun fail(reason: String): MediaCodec? {
        Log.e(TAG, "video decoder did not start: $reason")
        if (reason != lastFailure) {
            lastFailure = reason
            onFailed(reason)
        }
        return null
    }

    private fun start(data: ByteArray, offset: Int, length: Int): MediaCodec? {
        val csd = extractParameterSets(data, offset, length) ?: return fail("keyframe without SPS/PPS")
        val format = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, width, height).apply {
            setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, width * height)
            setInteger(MediaFormat.KEY_PRIORITY, 0)
            setInteger(MediaFormat.KEY_OPERATING_RATE, Short.MAX_VALUE.toInt())
            if (rotationDegrees != 0) setInteger(MediaFormat.KEY_ROTATION, rotationDegrees)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
            // Vendor switches that stop Qualcomm/MediaTek decoders from holding frames back.
            setInteger("vendor.qti-ext-dec-low-latency.enable", 1)
            setInteger("vendor.qti-ext-dec-picture-order.enable", 1)
            setInteger("vendor.low-latency.enable", 1)
        }
        val c = createConfigured(format) ?: return fail(configureError)
        val lowLatency = Build.VERSION.SDK_INT >= Build.VERSION_CODES.R &&
            c.codecInfo.getCapabilitiesForType(MediaFormat.MIMETYPE_VIDEO_AVC)
                .isFeatureSupported(MediaCodecInfo.CodecCapabilities.FEATURE_LowLatency)
        try {
            c.start()
            // SPS/PPS go in first as codec config.
            val index = c.dequeueInputBuffer(500_000)
            if (index < 0) {
                c.release()
                return fail("${c.name} gave no input buffer")
            }
            c.getInputBuffer(index)!!.apply { clear(); put(csd) }
            c.queueInputBuffer(index, 0, csd.size, 0, MediaCodec.BUFFER_FLAG_CODEC_CONFIG)
        } catch (e: Exception) {
            c.release()
            return fail("${c.name} failed to start: $e")
        }
        lastFailure = ""

        codec = c
        running = true
        outputThread = Thread({ renderLoop(c) }, "decoder-output").also { it.start() }
        Log.i(TAG, "decoder started ${width}x$height (${c.name})")
        onStarted("${c.name}, ${width}x$height, low-latency feature: ${if (lowLatency) "yes" else "no"}")
        return c
    }

    private var configureError = "no H.264 decoder found"

    /**
     * Prefers a vendor decoder variant built for low latency (e.g. Qualcomm's
     * "c2.qti.avc.decoder.low_latency", which Android never picks by default),
     * then the platform default, then any other H.264 decoder. Each gets the
     * low-latency format first, then a plain one: some (e.g. on Huawei Kirin chips)
     * reject the latency hints or vendor keys.
     */
    private fun createConfigured(format: MediaFormat): MediaCodec? {
        val avc = MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos.filter { info ->
            !info.isEncoder && !info.name.contains("secure", ignoreCase = true) &&
                info.supportedTypes.any { it.equals(MediaFormat.MIMETYPE_VIDEO_AVC, ignoreCase = true) }
        }
        val lowLatency = avc.filter { info ->
            info.name.contains("low_latency", ignoreCase = true) &&
                info.getCapabilitiesForType(MediaFormat.MIMETYPE_VIDEO_AVC).videoCapabilities
                    ?.isSizeSupported(width, height) == true
        }.map { it.name }
        val others = avc.map { it.name }.filter { it !in lowLatency }
        val plain = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, width, height).apply {
            setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, width * height)
            if (rotationDegrees != 0) setInteger(MediaFormat.KEY_ROTATION, rotationDegrees)
        }
        for (name in lowLatency + listOf<String?>(null) + others) {
            val c = try {
                if (name != null) MediaCodec.createByCodecName(name)
                else MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
            } catch (e: Exception) {
                configureError = "could not create ${name ?: "the default H.264 decoder"}: $e"
                continue
            }
            for ((i, f) in listOf(format, plain).withIndex()) {
                try {
                    if (i > 0) c.reset()
                    c.configure(f, surface, null, 0)
                    if (i > 0) Log.i(TAG, "${c.name} accepted only the plain format")
                    return c
                } catch (e: Exception) {
                    Log.w(TAG, "${c.name} rejected the ${if (i == 0) "low-latency" else "plain"} format", e)
                    configureError = "${c.name} rejected ${width}x$height: $e"
                }
            }
            c.release()
        }
        return null
    }

    private fun renderLoop(c: MediaCodec) {
        val info = MediaCodec.BufferInfo()
        while (running) {
            val index = try {
                c.dequeueOutputBuffer(info, 20_000)
            } catch (e: Exception) {
                break
            }
            if (index >= 0) {
                val queued = queuedAt.remove(info.presentationTimeUs)
                // Render immediately.
                try { c.releaseOutputBuffer(index, true) } catch (e: Exception) { break }
                if (queued != null) {
                    val (received, enqueued) = queued
                    onDecoded(info.presentationTimeUs, ((System.nanoTime() - enqueued) / 1000).toInt(),
                              ((enqueued - received) / 1000).toInt())
                }
            }
        }
    }

    fun release() {
        running = false
        outputThread?.join(500)
        outputThread = null
        codec?.let {
            try { it.stop() } catch (_: Exception) {}
            it.release()
        }
        codec = null
    }

    companion object {
        private const val TAG = "Tandem"

        /**
         * Largest picture with the shape of a [w] x [h] screen that the AVC decoder we
         * would use accepts, so the host can size the stream to fit. Width and height
         * limits alone are not enough: a decoder taking 4096 wide and 4096 tall may
         * still stop at 4096x2176 in total. Prefers the low-latency variants, like
         * createConfigured.
         */
        fun maxSize(w: Int, h: Int): Pair<Int, Int> {
            val avc = MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos.filter { info ->
                !info.isEncoder && !info.name.contains("secure", ignoreCase = true) &&
                    info.supportedTypes.any { it.equals(MediaFormat.MIMETYPE_VIDEO_AVC, ignoreCase = true) }
            }
            val best = avc.firstOrNull { it.name.contains("low_latency", ignoreCase = true) } ?: avc.firstOrNull()
            val caps = best?.getCapabilitiesForType(MediaFormat.MIMETYPE_VIDEO_AVC)?.videoCapabilities
                ?: return Pair(1920, 1080)
            // Scale the screen size up (2x at most: the host never sends more than the
            // monitor it captures) or down until the decoder takes it.
            var scale = 2.0
            while (scale > 0.1) {
                val sw = (w * scale).toInt() and 15.inv()
                val sh = (h * scale).toInt() and 15.inv()
                if (sw > 0 && sh > 0 && caps.isSizeSupported(sw, sh)) return Pair(sw, sh)
                scale -= 0.02
            }
            return Pair(caps.supportedWidths.upper, caps.supportedHeights.upper)
        }

        /** Copies SPS (type 7) and PPS (type 8) NAL units, with start codes, out of an Annex-B frame. */
        fun extractParameterSets(data: ByteArray, offset: Int, length: Int): ByteArray? {
            val end = offset + length
            val starts = ArrayList<Int>()
            var i = offset
            while (i + 3 <= end) {
                if (data[i].toInt() == 0 && data[i + 1].toInt() == 0 &&
                    (data[i + 2].toInt() == 1 || (i + 4 <= end && data[i + 2].toInt() == 0 && data[i + 3].toInt() == 1))
                ) {
                    starts.add(i)
                    i += if (data[i + 2].toInt() == 1) 3 else 4
                } else {
                    i++
                }
            }
            val out = java.io.ByteArrayOutputStream()
            for ((n, s) in starts.withIndex()) {
                val headerLen = if (data[s + 2].toInt() == 1) 3 else 4
                val nalType = data[s + headerLen].toInt() and 0x1f
                val stop = if (n + 1 < starts.size) starts[n + 1] else end
                if (nalType == 7 || nalType == 8) out.write(data, s, stop - s)
            }
            return if (out.size() > 0) out.toByteArray() else null
        }
    }
}
