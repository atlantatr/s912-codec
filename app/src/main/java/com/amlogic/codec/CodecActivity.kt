package com.amlogic.codec

import android.app.Activity
import android.os.Bundle
import android.view.Gravity
import android.view.SurfaceHolder
import android.view.Surface
import android.view.View
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.EditText

/**
 * "4K CI" oynatici iskeleti - SRT + 4K HEVC (S912 HW decode).
 *
 * Yerlesim:
 *   FrameLayout (MATCH x MATCH)
 *   ├─ SurfaceView   <- 4K HEVC / H264 HW decode (MediaCodec -> Surface)
 *   └─ LinearLayout (alt panel, URL + Play/Stop + status)
 *
 * 7.1.2 / S912 icin: HW HEVC decode MediaCodec ("c2.hevc" / "OMX.AMLogic.hevc.decode")
 * - S912, 4K HEVC 10-bit + Dolby Vision (profil 5/8) destekler
 */
class CodecActivity : Activity() {

    private lateinit var player: SrtCodecPlayer
    private lateinit var sv: android.view.SurfaceView
    private lateinit var tvStatus: TextView
    private lateinit var etUrl: EditText
    private lateinit var btnPlay: Button
    private lateinit var btnStop: Button
    private lateinit var btnPass: Button
    private var usePass = true   // DVB/Dolby-DOS icin default passthrough
    private var surface: Surface? = null

    private val holderCallback = object : SurfaceHolder.Callback {
        override fun surfaceCreated(holder: SurfaceHolder) {
            surface = holder.surface
        }
        override fun surfaceChanged(holder: SurfaceHolder, format: Int, w: Int, h: Int) {}
        override fun surfaceDestroyed(holder: SurfaceHolder) {
            surface = null
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        player = SrtCodecPlayer()

        sv = SurfaceView(this)
        sv.holder.addCallback(holderCallback)
        val videoLayer = FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT,
            FrameLayout.LayoutParams.MATCH_PARENT, Gravity.CENTER)

        val panel = LinearLayout(this)
        panel.orientation = LinearLayout.VERTICAL
        panel.setPadding(32, 0, 32, 32)

        tvStatus = TextView(this)
        tvStatus.text = "Hazir. SRT URL yaz ve Play."
        panel.addView(tvStatus)

        etUrl = EditText(this)
        etUrl.hint = "srt://192.168.1.50:9000?mode=caller"
        panel.addView(etUrl)

        // DVB/Dolby-DTS icin passthrough switch (PCM <--> PassThrough)
        val row = LinearLayout(this)
        row.orientation = LinearLayout.HORIZONTAL
        btnPlay = Button(this); btnPlay.text = "Play"
        btnStop = Button(this); btnStop.text = "Stop"; btnStop.isEnabled = false
        btnPass = Button(this); btnPass.text = "Passthrough"
        btnPass.setOnClickListener {
            usePass = !usePass
            btnPass.text = if (usePass) "PCM" else "Passthrough"
        }
        row.addView(btnPlay)
        row.addView(btnStop)
        row.addView(btnPass)
        panel.addView(row)

        val root = FrameLayout(this)
        root.addView(sv, videoLayer)
        root.addView(panel, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT,
            FrameLayout.LayoutParams.WRAP_CONTENT,
            Gravity.BOTTOM).apply {
            setMargins(0, 0, 0, 48)
            setPadding(48, 24, 48, 24)
        })

        btnPlay.setOnClickListener { onPlay() }
        btnStop.setOnClickListener { onStop() }

        setContentView(root)
    }

    private fun onPlay() {
        val url = etUrl.text.toString().trim()
        if (url.isEmpty()) { tvStatus.text = "URL bos."; return }
        tvStatus.text = "Aciyor: $url"
        if (!player.open(url)) { tvStatus.text = "Acilamadi (SRT/demuxer hata)."; return }

        val s = surface
        if (s != null) {
            player.startVideo(s)   // 4K HEVC HW decode -> Surface
            tvStatus.text = "Video: HW decode (S912) $url"
        } else {
            tvStatus.text = "Sadece audio (Surface hazir degil)"
        }
        // Audio: passthrough (DVB/Dolby-DTS) veya PCM decode
        if (usePass) {
            player.startPassThrough()
            tvStatus.text = "Audio: passthrough (offload) - $url"
        } else {
            player.start()
            tvStatus.text = "Audio: PCM decode - $url"
        }
        btnPlay.isEnabled = false
        btnStop.isEnabled = true
    }

    private fun onStop() {
        player.release()
        tvStatus.text = "Durduruldu."
        btnPlay.isEnabled = true
        btnStop.isEnabled = false
    }

    override fun onDestroy() {
        player.release()
        super.onDestroy()
    }
}
