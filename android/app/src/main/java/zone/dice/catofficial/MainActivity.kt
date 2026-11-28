package zone.dice.catofficial

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
import android.provider.MediaStore
import android.provider.Settings
import android.util.Base64
import android.view.ViewGroup
import android.webkit.CookieManager
import android.webkit.JavascriptInterface
import android.webkit.URLUtil
import android.webkit.ValueCallback
import android.webkit.WebChromeClient
import android.webkit.WebResourceRequest
import android.webkit.WebSettings
import android.webkit.WebView
import android.webkit.WebViewClient
import android.widget.FrameLayout
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

class MainActivity : AppCompatActivity() {
    private lateinit var web: WebView
    private var fileCallback: ValueCallback<Array<Uri>>? = null
    private var loadedPanel = false
    private var insetScript = ""

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

    @SuppressLint("SetJavaScriptEnabled", "AddJavascriptInterface")
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
            addJavascriptInterface(JsBridge(), "CatOfficial")
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
        val root = FrameLayout(this).apply {
            setBackgroundColor(0xFF111827.toInt())
            addView(web)
        }
        ViewCompat.setOnApplyWindowInsetsListener(root) { _, insets ->
            applyWebInsets(insets)
            insets
        }
        setContentView(root)
        web.loadDataWithBaseURL(
            null,
            "<html><body style='background:#111827;color:#94a3b8;font-family:sans-serif;padding:2rem'>CatOfficial 正在启动…</body></html>",
            "text/html",
            "utf-8",
            null,
        )
        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() {
                if (web.canGoBack()) web.goBack() else moveTaskToBack(true)
            }
        })
        web.post(pollPanel)
    }

    private val pollPanel = object : Runnable {
        override fun run() {
            if (isDestroyed || loadedPanel) return
            if (!Native.available) {
                loadedPanel = true
                web.loadDataWithBaseURL(
                    null,
                    "<html><body style='background:#111827;color:#fca5a5;font-family:sans-serif;padding:2rem'>缺少 libcatofficial.so。请先运行 scripts/build-android.ps1，再重新编译 APK。</body></html>",
                    "text/html",
                    "utf-8",
                    null,
                )
                return
            }
            val running = try {
                Native.isRunning()
            } catch (e: UnsatisfiedLinkError) {
                loadedPanel = true
                web.loadDataWithBaseURL(
                    null,
                    "<html><body style='background:#111827;color:#fca5a5;font-family:sans-serif;padding:2rem'>无法加载本地核心库。</body></html>",
                    "text/html",
                    "utf-8",
                    null,
                )
                return
            }
            val url = File(filesDir, ".panel-url").takeIf { it.isFile }?.readText()?.trim().orEmpty()
            if (url.startsWith("http://") && running) {
                loadedPanel = true
                web.loadUrl(url)
            } else {
                web.postDelayed(this, 300)
            }
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
        val prefs = getSharedPreferences("catofficial", MODE_PRIVATE)
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
        super.onDestroy()
    }

    companion object {
        private const val THEME_SYNC_SCRIPT = """
            (function(){
              if (window.__catofficialThemeSync) return;
              window.__catofficialThemeSync = true;
              function sync(){
                try {
                  CatOfficial.setDark(document.documentElement.classList.contains('dark'));
                } catch (e) {}
              }
              sync();
              new MutationObserver(sync).observe(document.documentElement, {attributes:true, attributeFilter:['class']});
            })();
        """
    }
}
