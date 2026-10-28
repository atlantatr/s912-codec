package com.amlogic.codec;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
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
    private lateinit var btnDvb: Button

    private var usePass = true
    private var surface: Surface? = null
    private val ui = Handler(Looper.getMainLooper())
    private var lastUrl = ""

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

        btnDvb = Button(this).apply {
            text = "TV: TRT 4K"
            setOnClickListener { onPlayDvb() }
        }

        row.addView(btnPlay)
        row.addView(btnStop)
        row.addView(btnPass)
        row.addView(btnDvb)
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

        // API 23+: /sdcard okumak icin izin iste (targetSdk 25).
        if (android.os.Build.VERSION.SDK_INT >= 23 &&
            checkSelfPermission(android.Manifest.permission.READ_EXTERNAL_STORAGE) !=
            android.content.pm.PackageManager.PERMISSION_GRANTED
        ) {
            requestPermissions(arrayOf(android.Manifest.permission.READ_EXTERNAL_STORAGE), 1)
        }

        handleAutoPlay(intent?.getStringExtra("url"))
    }

    private fun handleAutoPlay(autoUrl: String?) {
        if (autoUrl.isNullOrBlank()) {
            tvStatus.text = audioStatus("Hazir. SRT URL yaz ve Play.")
            return
        }
        etUrl.setText(autoUrl)
        tvStatus.text = "Otomatik aciliyor: $autoUrl"
        // Surface'in hazir olmasini bekle.
        ui.postDelayed({ if (!isFinishing) onPlay() }, 1500)
    }

    override fun onNewIntent(intent: android.content.Intent?) {
        super.onNewIntent(intent)
        setIntent(intent)
        handleAutoPlay(intent?.getStringExtra("url"))
    }

    /** digital_raw / HDMI durumunu tek satirda ozetler. */
    private fun audioStatus(prefix: String): String {
        val raw = AmlogicRawAudio.readRawMode()
        val mode = when (raw) {
            1 -> "RAW (bitstream)"
            0 -> "PCM"
            else -> "okunamadi"
        }
        val avail = if (AmlogicRawAudio.isAvailable()) "" else " [dugum yazilamaz]"
        return "$prefix\nDijital cikis: $mode$avail"
    }

    /**
     * TRT 4K (Turksat 42.0E, 11767 V, SR 15000).
     * Tuner islemi kilit bekledigi icin arka planda yapilir.
     */
    private fun onPlayDvb() {
        val freq = 11_767_000_000L
        val sr = 15000
        lastUrl = "DVB-S2 11767 V $sr (TRT 4K)"
        tvStatus.text = "TV: tune ediliyor..."
        btnPlay.isEnabled = false
        btnDvb.isEnabled = false
        btnStop.isEnabled = true

        Thread {
            val ok = player.openDvb(freq, true, sr)
            ui.post {
                if (isFinishing) return@post
                if (!ok) {
                    tvStatus.text = "TV: kilit yok / akis gelmedi (11767 V $sr)"
                    btnPlay.isEnabled = true
                    btnDvb.isEnabled = true
                    btnStop.isEnabled = false
                    return@post
                }
                surface?.let { player.startVideo(it) }
                if (usePass) player.startPassThrough() else player.start()
                ui.removeCallbacks(statusTick)
                ui.post(statusTick)
            }
        }.start()
    }

    private fun onPlay() {
        val url = etUrl.text.toString().trim()
        if (url.isEmpty()) {
            tvStatus.text = "URL bos."
            return
        }

        lastUrl = url
        tvStatus.text = "Aciyor: $url"
        if (!player.open(url)) {
            tvStatus.text = "Acilamadi (SRT/demuxer hatasi)."
            return
        }

        val s = surface
        if (s != null) {
            player.startVideo(s)
        }

        if (usePass) {
            player.startPassThrough()
        } else {
            player.start()
        }

        btnPlay.isEnabled = false
        btnStop.isEnabled = true

        // digital_raw degisimini canli goster.
        ui.removeCallbacks(statusTick)
        ui.post(statusTick)
    }

    private val statusTick = object : Runnable {
        override fun run() {
            if (isFinishing) return
            tvStatus.text = audioStatus("Oynatiliyor: $lastUrl")
            ui.postDelayed(this, 1000)
        }
    }

    private fun stopPlayback() {
        ui.removeCallbacks(statusTick)
        player.release()
        player = SrtCodecPlayer()   // release() native baglami kapatir, yeniden kullanilamaz
        tvStatus.text = audioStatus("Durduruldu.")
        btnPlay.isEnabled = true
        btnDvb.isEnabled = true
        btnStop.isEnabled = false
    }

    override fun onDestroy() {
        ui.removeCallbacks(statusTick)
        player.release()
        super.onDestroy()
    }
}
