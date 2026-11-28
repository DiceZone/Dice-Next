package zone.dice.dicenext

import android.app.Notification
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.net.ConnectivityManager
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import android.os.Process
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import kotlin.concurrent.thread

/**
 * 前台服务里跑 C++ 核心。适配器连接挂在这个进程上，
 * 所以必须常驻，否则一进后台 Android 会把骰子一起杀掉。
 */
class CoreService : Service() {
    companion object {
        const val CHANNEL_ID = "dicenext-core"
        const val NOTIFICATION_ID = 1
        const val RESTART_EXIT = 42
    }

    private var worker: Thread? = null
    private var wakeLock: PowerManager.WakeLock? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        AssetPack.sync(this)
        val pm = getSystemService(POWER_SERVICE) as PowerManager
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "dicenext:core").apply {
            setReferenceCounted(false)
            acquire()
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        ServiceCompat.startForeground(
            this,
            NOTIFICATION_ID,
            notification(),
            if (Build.VERSION.SDK_INT >= 34) ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE else 0,
        )
        if (!Native.available) {
            stopSelf()
            return START_NOT_STICKY
        }
        if (worker?.isAlive != true && !Native.isRunning()) {
            worker = thread(name = "dice-next-core", isDaemon = false) {
                val code = try {
                    Native.initDns(getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager)
                    Native.run(filesDir.absolutePath)
                } catch (e: UnsatisfiedLinkError) {
                    stopSelf()
                    return@thread
                }
                if (code == RESTART_EXIT) {
                    Process.killProcess(Process.myPid())
                } else {
                    stopSelf()
                }
            }
        }
        return START_STICKY
    }

    override fun onDestroy() {
        if (Native.available) Native.stop()
        wakeLock?.let { if (it.isHeld) it.release() }
        wakeLock = null
        super.onDestroy()
    }

    private fun notification(): Notification {
        val launch = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle(getString(R.string.app_name))
            .setContentText(getString(R.string.notification_running))
            .setSmallIcon(R.drawable.ic_notification)
            .setContentIntent(launch)
            .setOngoing(true)
            .setForegroundServiceBehavior(NotificationCompat.FOREGROUND_SERVICE_IMMEDIATE)
            .build()
    }
}
