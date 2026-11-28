package zone.dice.dicenext

import android.Manifest
import android.annotation.SuppressLint
import android.app.DownloadManager
import android.content.Intent
import android.net.Uri
import android.content.pm.ApplicationInfo
import android.graphics.Color
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.os.PowerManager
import android.os.SystemClock
import android.provider.MediaStore
import android.provider.Settings
import android.util.Base64
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.webkit.CookieManager
import android.webkit.JavascriptInterface
import android.webkit.URLUtil
import android.webkit.ValueCallback
import android.webkit.WebChromeClient
import android.webkit.WebResourceError
import android.webkit.WebResourceRequest
import android.webkit.WebResourceResponse
import android.webkit.WebSettings
import android.webkit.WebView
import android.webkit.WebViewClient
import android.widget.FrameLayout
import android.widget.TextView
import android.widget.Toast
import androidx.activity.OnBackPressedCallback
import androidx.activity.result.PickVisualMediaRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.result.contract.ActivityResultContracts.PickVisualMedia
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import androidx.core.view.ViewCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.webkit.WebSettingsCompat
import androidx.webkit.WebViewFeature
import java.io.File
import java.net.HttpURLConnection
import java.net.Proxy
import java.net.URL
import java.util.concurrent.Executors

class MainActivity : AppCompatActivity() {
    private lateinit var web: WebView
    private lateinit var splashWeb: WebView
    private lateinit var splashError: TextView
    private var fileCallback: ValueCallback<Array<Uri>>? = null
    private var loadedPanel = false
    private var startedLoad = false
    private var introDone = false
    private var panelPageReady = false
    private var splashFading = false
    private var pollStartedAt = 0L
    private var insetScript = ""
    private val probeExecutor = Executors.newSingleThreadExecutor()
    private val introBackup = Runnable { markIntroDone() }

    private val pickFiles = registerForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris ->
        deliverFiles(uris)
    }
    private val pickFile = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        deliverFiles(if (uri != null) listOf(uri) else emptyList())
    }
    private val pickImage = registerForActivityResult(PickVisualMedia()) { uri ->
        deliverFiles(if (uri != null) listOf(uri) else emptyList())
    }
    private val pickImages = registerForActivityResult(ActivityResultContracts.PickMultipleVisualMedia()) { uris ->
        deliverFiles(uris)
    }
    private val pickGallery = registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
        val data = result.data
        val uris = ArrayList<Uri>()
        val clip = data?.clipData
        if (clip != null) {
            for (i in 0 until clip.itemCount) clip.getItemAt(i).uri?.let(uris::add)
        } else {
            data?.data?.let(uris::add)
        }
        deliverFiles(uris)
    }

    private fun deliverFiles(uris: List<Uri>) {
        fileCallback?.onReceiveValue(if (uris.isEmpty()) null else uris.toTypedArray())
        fileCallback = null
    }

    private fun mimeTypes(params: WebChromeClient.FileChooserParams?): Array<String> {
        val types = params?.acceptTypes.orEmpty()
            .flatMap { it.split(',', ';') }
            .map { it.trim() }
            .filter { it.isNotEmpty() }
            .map { t ->
                when (t.lowercase()) {
                    ".png" -> "image/png"
                    ".jpg", ".jpeg" -> "image/jpeg"
                    ".gif" -> "image/gif"
                    ".webp" -> "image/webp"
                    ".zip" -> "application/zip"
                    ".gz", ".tgz", ".tar.gz" -> "application/gzip"
                    else -> t
                }
            }
            .distinct()
        return if (types.isEmpty()) arrayOf("*/*") else types.toTypedArray()
    }

    private fun isImageOnly(types: Array<String>): Boolean =
        types.isNotEmpty() && types.none { it == "*/*" } && types.all { it.startsWith("image/") }

    private fun launchImagePicker(multiple: Boolean) {
        if (PickVisualMedia.isPhotoPickerAvailable(this)) {
            val request = PickVisualMediaRequest(PickVisualMedia.ImageOnly)
            if (multiple) pickImages.launch(request) else pickImage.launch(request)
            return
        }
        val pick = Intent(Intent.ACTION_PICK, MediaStore.Images.Media.EXTERNAL_CONTENT_URI).apply {
            type = "image/*"
            putExtra(Intent.EXTRA_ALLOW_MULTIPLE, multiple)
        }
        try {
            pickGallery.launch(pick)
        } catch (_: Exception) {
            pickGallery.launch(
                Intent(Intent.ACTION_GET_CONTENT).apply {
                    addCategory(Intent.CATEGORY_OPENABLE)
                    type = "image/*"
                    putExtra(Intent.EXTRA_ALLOW_MULTIPLE, multiple)
                },
            )
        }
    }

    private val notifications = registerForActivityResult(ActivityResultContracts.RequestPermission()) { }

    @SuppressLint("SetJavaScriptEnabled", "AddJavascriptInterface", "ClickableViewAccessibility")
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        window.statusBarColor = Color.TRANSPARENT
        window.navigationBarColor = Color.TRANSPARENT
        if (Build.VERSION.SDK_INT >= 29) {
            window.isStatusBarContrastEnforced = false
            window.isNavigationBarContrastEnforced = false
        }
        applySystemChrome(dark = true)
        if (Build.VERSION.SDK_INT >= 33) notifications.launch(Manifest.permission.POST_NOTIFICATIONS)
        ContextCompat.startForegroundService(this, Intent(this, CoreService::class.java))
        maybeIgnoreBatteryOptimizations()
        if (applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE != 0) {
            WebView.setWebContentsDebuggingEnabled(true)
        }

        web = WebView(this).apply {
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT,
            )
            settings.javaScriptEnabled = true
            settings.domStorageEnabled = true
            settings.databaseEnabled = true
            settings.allowFileAccess = false
            settings.mixedContentMode = WebSettings.MIXED_CONTENT_COMPATIBILITY_MODE
            settings.mediaPlaybackRequiresUserGesture = false
            if (WebViewFeature.isFeatureSupported(WebViewFeature.ALGORITHMIC_DARKENING)) {
                WebSettingsCompat.setAlgorithmicDarkeningAllowed(settings, true)
            }
            CookieManager.getInstance().setAcceptCookie(true)
            CookieManager.getInstance().setAcceptThirdPartyCookies(this, true)
            setBackgroundColor(SPLASH_BG)
            addJavascriptInterface(JsBridge(), "DiceNext")
            webViewClient = object : WebViewClient() {
                override fun shouldOverrideUrlLoading(view: WebView, request: WebResourceRequest): Boolean {
                    val host = request.url.host
                    return if (host == "127.0.0.1" || host == "localhost") false else {
                        openExternal(request.url.toString())
                        true
                    }
                }

                override fun onPageFinished(view: WebView?, url: String?) {
                    pushInsetsToWeb()
                    view?.evaluateJavascript(THEME_SYNC_SCRIPT, null)
                    view?.evaluateJavascript(DROP_SW_SCRIPT, null)
                    revealPanelIfReady(view)
                }

                override fun onReceivedError(
                    view: WebView,
                    request: WebResourceRequest,
                    error: WebResourceError,
                ) {
                    if (request.isForMainFrame) retryPanelLoad()
                }

                override fun onReceivedHttpError(
                    view: WebView,
                    request: WebResourceRequest,
                    errorResponse: WebResourceResponse,
                ) {
                    if (request.isForMainFrame && errorResponse.statusCode >= 500) retryPanelLoad()
                }
            }
            webChromeClient = object : WebChromeClient() {
                override fun onShowFileChooser(
                    view: WebView?,
                    callback: ValueCallback<Array<Uri>>?,
                    params: FileChooserParams?,
                ): Boolean {
                    fileCallback?.onReceiveValue(null)
                    fileCallback = callback
                    val types = mimeTypes(params)
                    val multiple = params?.mode == FileChooserParams.MODE_OPEN_MULTIPLE
                    return try {
                        if (isImageOnly(types)) launchImagePicker(multiple) else {
                            if (multiple) pickFiles.launch(types) else pickFile.launch(types)
                        }
                        true
                    } catch (_: Exception) {
                        fileCallback?.onReceiveValue(null)
                        fileCallback = null
                        false
                    }
                }
            }
            setDownloadListener { url, userAgent, contentDisposition, mime, _ ->
                download(url, userAgent, contentDisposition, mime)
            }
        }
        val pad = (24 * resources.displayMetrics.density).toInt()
        splashWeb = WebView(this).apply {
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT,
            )
            setBackgroundColor(SPLASH_BG)
            setLayerType(View.LAYER_TYPE_HARDWARE, null)
            isVerticalScrollBarEnabled = false
            isHorizontalScrollBarEnabled = false
            overScrollMode = View.OVER_SCROLL_NEVER
            isClickable = true
            isFocusable = false
            settings.javaScriptEnabled = true
            settings.allowFileAccess = true
            settings.allowContentAccess = false
            settings.domStorageEnabled = false
            settings.databaseEnabled = false
            addJavascriptInterface(SplashBridge(), "Splash")
            webViewClient = object : WebViewClient() {
                override fun shouldOverrideUrlLoading(view: WebView, request: WebResourceRequest): Boolean = true

                override fun onPageFinished(view: WebView?, url: String?) {
                    view?.removeCallbacks(introBackup)
                    view?.postDelayed(introBackup, INTRO_MS)
                }
            }
            setOnTouchListener { _, _ -> true }
            val intro = resources.openRawResource(R.raw.dn_splash).bufferedReader().use { it.readText() }
            loadDataWithBaseURL(null, intro, "text/html", "utf-8", null)
        }
        splashError = TextView(this).apply {
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT,
            )
            gravity = Gravity.CENTER
            textAlignment = View.TEXT_ALIGNMENT_CENTER
            setTextColor(0xFFFCA5A5.toInt())
            textSize = 16f
            setPadding(pad, pad, pad, pad)
            setBackgroundColor(SPLASH_BG)
            visibility = View.GONE
        }
        val root = FrameLayout(this).apply {
            setBackgroundColor(SPLASH_BG)
            addView(web)
            addView(splashWeb)
            addView(splashError)
        }
        ViewCompat.setOnApplyWindowInsetsListener(root) { _, insets ->
            applyWebInsets(insets)
            insets
        }
        setContentView(root)
        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() {
                if (web.canGoBack() && isPanelUrl(previousHistoryUrl())) {
                    web.goBack()
                    return
                }
                if (loadedPanel && !isPanelUrl(web.url)) {
                    val url = panelUrl()
                    if (url.isNotEmpty()) {
                        hideSplashImmediate()
                        web.loadUrl(url)
                        return
                    }
                }
                moveTaskToBack(true)
            }
        })
        splashWeb.postDelayed(introBackup, INTRO_FAILSAFE_MS)
        web.post(pollPanel)
    }

    private fun isPanelUrl(url: String?): Boolean {
        if (url.isNullOrEmpty()) return false
        val lower = url.lowercase()
        return lower.startsWith("http://127.0.0.1") || lower.startsWith("http://localhost")
    }

    private fun previousHistoryUrl(): String? {
        val hist = web.copyBackForwardList()
        val idx = hist.currentIndex
        if (idx <= 0) return null
        return hist.getItemAtIndex(idx - 1)?.url
    }

    private fun panelUrl(): String =
        File(filesDir, ".panel-url").takeIf { it.isFile }?.readText()?.trim().orEmpty()

    private fun showSplashError(message: String) {
        splashError.text = message
        splashError.visibility = View.VISIBLE
        splashError.alpha = 1f
        splashWeb.visibility = View.VISIBLE
    }

    private fun showSplashWaiting(timeout: Boolean) {
        if (splashFading || loadedPanel) return
        splashError.visibility = View.GONE
        splashWeb.visibility = View.VISIBLE
        splashWeb.alpha = 1f
        val status = (if (timeout) getString(R.string.splash_timeout) else "正在启动…")
            .replace("\\", "\\\\")
            .replace("'", "\\'")
        splashWeb.evaluateJavascript(
            "(function(){var e=document.getElementById('dn-status');if(e)e.textContent='$status';})()",
            null,
        )
    }

    private fun retryPanelLoad() {
        if (isDestroyed || loadedPanel || splashFading) return
        startedLoad = false
        panelPageReady = false
        showSplashWaiting(timeout = false)
        web.removeCallbacks(pollPanel)
        web.postDelayed(pollPanel, 400)
    }

    private fun markIntroDone() {
        if (isDestroyed || introDone) return
        introDone = true
        maybeRevealPanel()
    }

    private fun revealPanelIfReady(view: WebView?) {
        if (view == null || isDestroyed || loadedPanel || splashFading) return
        view.evaluateJavascript(OFFLINE_TITLE_SCRIPT) { raw ->
            if (isDestroyed || loadedPanel || splashFading) return@evaluateJavascript
            val title = raw.orEmpty().trim().trim('"')
            if (title.contains("无法连接") || title.contains("Cannot reach")) {
                retryPanelLoad()
                return@evaluateJavascript
            }
            panelPageReady = true
            maybeRevealPanel()
        }
    }

    private fun maybeRevealPanel() {
        if (isDestroyed || loadedPanel || splashFading) return
        if (!introDone || !panelPageReady) return
        loadedPanel = true
        splashFading = true
        splashError.visibility = View.GONE
        splashWeb.animate()
            .alpha(0f)
            .setDuration(FADE_MS)
            .withEndAction {
                hideSplashImmediate()
            }
            .start()
    }

    private fun hideSplashImmediate() {
        splashWeb.animate().cancel()
        splashWeb.visibility = View.GONE
        splashWeb.alpha = 0f
        splashError.visibility = View.GONE
        splashFading = false
    }

    private fun panelHttpReady(url: String): Boolean {
        return try {
            val probe = url.trimEnd('/') + "/api/system/status"
            val conn = URL(probe).openConnection(Proxy.NO_PROXY) as HttpURLConnection
            try {
                conn.connectTimeout = 600
                conn.readTimeout = 800
                conn.instanceFollowRedirects = false
                conn.useCaches = false
                conn.requestMethod = "GET"
                conn.setRequestProperty("Accept", "application/json")
                conn.setRequestProperty("Connection", "close")
                conn.responseCode in 200..499
            } finally {
                conn.disconnect()
            }
        } catch (_: Exception) {
            false
        }
    }

    private val pollPanel = object : Runnable {
        override fun run() {
            if (isDestroyed || startedLoad || loadedPanel) return
            if (!Native.available) {
                showSplashError(getString(R.string.splash_missing_lib))
                return
            }
            val running = try {
                Native.isRunning()
            } catch (_: UnsatisfiedLinkError) {
                showSplashError(getString(R.string.splash_load_failed))
                return
            }
            if (pollStartedAt == 0L) pollStartedAt = SystemClock.elapsedRealtime()
            val elapsed = SystemClock.elapsedRealtime() - pollStartedAt
            if (elapsed > 45_000L) showSplashWaiting(timeout = true)
            val url = panelUrl()
            val task = this
            probeExecutor.execute {
                val ready = url.startsWith("http://") && running && panelHttpReady(url)
                web.post {
                    if (isDestroyed || startedLoad || loadedPanel) return@post
                    if (ready) {
                        startedLoad = true
                        web.loadUrl(url)
                    } else {
                        web.postDelayed(task, 300)
                    }
                }
            }
        }
    }

    inner class SplashBridge {
        @JavascriptInterface
        fun onIntroDone() {
            runOnUiThread { markIntroDone() }
        }
    }

    inner class JsBridge {
        @JavascriptInterface
        fun openUrl(url: String) {
            runOnUiThread { openExternal(url) }
        }

        @JavascriptInterface
        fun setDark(dark: Boolean) {
            runOnUiThread { applySystemChrome(dark) }
        }
    }

    private fun applySystemChrome(dark: Boolean) {
        val controller = WindowInsetsControllerCompat(window, window.decorView)
        controller.isAppearanceLightStatusBars = !dark
        controller.isAppearanceLightNavigationBars = !dark
    }

    private fun applyWebInsets(insets: WindowInsetsCompat) {
        val bars = insets.getInsets(
            WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout(),
        )
        val d = resources.displayMetrics.density
        fun css(px: Int) = String.format("%.1fpx", px / d)
        insetScript = """
            (function(){
              var s=document.documentElement.style;
              s.setProperty('--app-inset-top','${css(bars.top)}');
              s.setProperty('--app-inset-right','${css(bars.right)}');
              s.setProperty('--app-inset-bottom','${css(bars.bottom)}');
              s.setProperty('--app-inset-left','${css(bars.left)}');
            })();
        """.trimIndent()
        pushInsetsToWeb()
    }

    private fun pushInsetsToWeb() {
        if (insetScript.isEmpty() || !::web.isInitialized) return
        web.evaluateJavascript(insetScript, null)
    }

    private fun openExternal(url: String) {
        val uri = try {
            Uri.parse(url)
        } catch (_: Exception) {
            return
        }
        val scheme = uri.scheme?.lowercase() ?: return
        if (scheme != "http" && scheme != "https" && scheme != "mqqapi" && scheme != "mqq") return
        val targets = ArrayList<Uri>()
        // q.qq.com 绑定页必须进 QQ 内置浏览器才会走登录；官方 jumpQQ 用 url_prefix=Base64，不是 url_content。
        if (scheme == "http" || scheme == "https") {
            val host = uri.host.orEmpty().lowercase()
            if (host == "q.qq.com" || host.endsWith(".q.qq.com") || host.endsWith(".qq.com")) {
                targets += qqForwardUri(url, encodedQuery = true)
                targets += qqForwardUri(url, encodedQuery = false)
            }
        }
        targets += uri
        for (target in targets) {
            val intent = Intent(Intent.ACTION_VIEW, target).apply {
                addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            }
            try {
                startActivity(intent)
                return
            } catch (_: Exception) {
            }
        }
        try {
            startActivity(
                Intent.createChooser(
                    Intent(Intent.ACTION_VIEW, uri).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
                    getString(R.string.open_url_chooser),
                ),
            )
        } catch (_: Exception) {
            Toast.makeText(this, getString(R.string.open_url_failed), Toast.LENGTH_LONG).show()
        }
    }

    /// 与 q.qq.com 绑定页自带的 jumpQQ 一致：内层参数整体 encode，Android 再加 plg_auth=1。
    private fun qqForwardUri(url: String, encodedQuery: Boolean): Uri {
        val prefix = Base64.encodeToString(url.toByteArray(Charsets.UTF_8), Base64.NO_WRAP)
        val inner = "src_type=web&version=1&url_prefix=$prefix&t=${System.currentTimeMillis()}"
        return if (encodedQuery) {
            Uri.parse("mqqapi://forward/url?${Uri.encode(inner)}&plg_auth=1")
        } else {
            Uri.parse("mqqapi://forward/url?$inner&plg_auth=1")
        }
    }

    private fun download(url: String, userAgent: String, contentDisposition: String, mime: String) {
        try {
            val name = URLUtil.guessFileName(url, contentDisposition, mime)
            val request = DownloadManager.Request(Uri.parse(url)).apply {
                addRequestHeader("User-Agent", userAgent)
                CookieManager.getInstance().getCookie(url)?.let { addRequestHeader("Cookie", it) }
                setMimeType(mime)
                setNotificationVisibility(DownloadManager.Request.VISIBILITY_VISIBLE_NOTIFY_COMPLETED)
                setDestinationInExternalPublicDir(Environment.DIRECTORY_DOWNLOADS, name)
                setTitle(name)
            }
            (getSystemService(DOWNLOAD_SERVICE) as DownloadManager).enqueue(request)
            Toast.makeText(this, getString(R.string.download_started, name), Toast.LENGTH_SHORT).show()
        } catch (e: Exception) {
            Toast.makeText(this, e.message ?: "download failed", Toast.LENGTH_LONG).show()
        }
    }

    private fun maybeIgnoreBatteryOptimizations() {
        val prefs = getSharedPreferences("dicenext", MODE_PRIVATE)
        if (prefs.getBoolean("asked_battery", false)) return
        val pm = getSystemService(POWER_SERVICE) as PowerManager
        if (pm.isIgnoringBatteryOptimizations(packageName)) return
        prefs.edit().putBoolean("asked_battery", true).apply()
        try {
            startActivity(
                Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS, Uri.parse("package:$packageName")),
            )
        } catch (_: Exception) {
        }
    }

    override fun onPause() {
        CookieManager.getInstance().flush()
        super.onPause()
    }

    override fun onDestroy() {
        web.removeCallbacks(pollPanel)
        if (::splashWeb.isInitialized) {
            splashWeb.removeCallbacks(introBackup)
            splashWeb.removeJavascriptInterface("Splash")
        }
        probeExecutor.shutdownNow()
        super.onDestroy()
    }

    companion object {
        // CSS: bloom 1.1s; faces 0.52s + delay ≤0.08s; particles 0.9s + delay ≤0.52s (~1.42s).
        // Wait 1.8s so the intro can finish even if HTTP is already up, then fade 480ms.
        private const val INTRO_MS = 1800L
        private const val INTRO_FAILSAFE_MS = 2800L
        private const val FADE_MS = 480L
        private val SPLASH_BG = 0xFF0A0E1C.toInt()
        private const val DROP_SW_SCRIPT = """
            (function(){
              if (!navigator.serviceWorker) return;
              navigator.serviceWorker.getRegistrations().then(function(rs){
                rs.forEach(function(r){ r.unregister(); });
              });
            })();
        """
        private const val OFFLINE_TITLE_SCRIPT =
            "(function(){var h=document.querySelector('h1');return h?String(h.textContent):'';})()"
        private const val THEME_SYNC_SCRIPT = """
            (function(){
              if (window.__dicenextThemeSync) return;
              window.__dicenextThemeSync = true;
              function sync(){
                try {
                  DiceNext.setDark(document.documentElement.classList.contains('dark'));
                } catch (e) {}
              }
              sync();
              new MutationObserver(sync).observe(document.documentElement, {attributes:true, attributeFilter:['class']});
            })();
        """
    }
}
