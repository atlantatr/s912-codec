package com.amlogic.codec;

import android.app.Activity;
import android.os.Bundle;
import android.view.Gravity;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

/**
 * SRT / DVB oynatici iskeleti - 4K HEVC (S912 HW decode).
 *
 * FrameLayout
 *   +-- SurfaceView          (MediaCodec HW decode)
 *   +-- alt panel (URL, Play/Stop, Passthrough/PCM, durum)
 */
class CodecActivity : Activity() {

    private lateinit var player: SrtCodecPlayer
    private lateinit var sv: android.view.SurfaceView
    private lateinit var tvStatus: TextView
    private lateinit var etUrl: EditText
    private lateinit var btnPlay: Button
    private lateinit var btnStop: Button
    private lateinit var btnPass: Button

    private var usePass = true
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

        sv = android.view.SurfaceView(this)
        sv.holder.addCallback(holderCallback)
        sv.setKeepScreenOn(true)

        val videoLayer = FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT,
            FrameLayout.LayoutParams.MATCH_PARENT,
            Gravity.CENTER
        )

        val panel = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(48, 24, 48, 24)
        }

        tvStatus = TextView(this).apply {
            text = "Hazir. SRT URL yaz ve Play."
        }
        panel.addView(tvStatus)

        etUrl = EditText(this).apply {
            hint = "srt://192.168.1.50:9000?mode=caller"
            setSingleLine(true)
        }
        panel.addView(etUrl)

        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
        }

        btnPlay = Button(this).apply { text = "Play" }
        btnStop = Button(this).apply {
            text = "Stop"
            isEnabled = false
        }
        btnPass = Button(this).apply {
            text = "Mode: Passthrough"
            setOnClickListener {
                usePass = !usePass
                text = if (usePass) "Mode: Passthrough" else "Mode: PCM"
            }
        }

        row.addView(btnPlay)
        row.addView(btnStop)
        row.addView(btnPass)
        panel.addView(row)

        val root = FrameLayout(this)
        root.addView(sv, videoLayer)
        root.addView(
            panel,
            FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.BOTTOM
            ).apply {
                setMargins(0, 0, 0, 24)
            }
        )

        btnPlay.setOnClickListener { onPlay() }
        btnStop.setOnClickListener { stopPlayback() }

        setContentView(root)
    }

    private fun onPlay() {
        val url = etUrl.text.toString().trim()
        if (url.isEmpty()) {
            tvStatus.text = "URL bos."
            return
        }

        tvStatus.text = "Aciyor: $url"
        if (!player.open(url)) {
            tvStatus.text = "Acilamadi (SRT/demuxer hatasi)."
            return
        }

        val s = surface
        if (s != null) {
            player.startVideo(s)
            tvStatus.text = "Video: HW decode - $url"
        } else {
            tvStatus.text = "Sadece audio (Surface hazir degil)"
        }

        if (usePass) {
            player.startPassThrough()
            tvStatus.text = "Audio: passthrough - $url"
        } else {
            player.start()
            tvStatus.text = "Audio: PCM decode - $url"
        }

        btnPlay.isEnabled = false
        btnStop.isEnabled = true
    }

    private fun stopPlayback() {
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
