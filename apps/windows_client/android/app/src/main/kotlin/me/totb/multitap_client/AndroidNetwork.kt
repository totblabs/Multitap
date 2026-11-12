package me.totb.multitap_client

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.os.Handler
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.EventChannel
import io.flutter.plugin.common.MethodChannel

/**
 * Network changes for a quick reconnect (as ToTBDesk 2.4.0/2.4.5 tuned it):
 * Dart marks the network a connection was made on; when that network is
 * lost (Wi-Fi gone, not merely another default such as Samsung's
 * intelligent Wi-Fi preferring mobile data while Wi-Fi still works) it hears
 * `lost` at once instead of waiting for a timeout, and `available` when a
 * validated default network is there to reconnect on. Only these two words
 * reach Dart: no addresses or network names.
 */
class AndroidNetwork(context: Context, engine: FlutterEngine) {
    private val manager = context.getSystemService(ConnectivityManager::class.java)
    private val main = Handler(context.mainLooper)
    private var sink: EventChannel.EventSink? = null
    @Volatile private var defaultNetwork: Network? = null
    @Volatile private var marked: Network? = null
    private var registered = false

    private val defaultCallback = object : ConnectivityManager.NetworkCallback() {
        override fun onAvailable(network: Network) { defaultNetwork = network }
        override fun onLost(network: Network) { if (defaultNetwork == network) defaultNetwork = null }
        override fun onCapabilitiesChanged(network: Network, capabilities: NetworkCapabilities) {
            defaultNetwork = network
            if (capabilities.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED) && network != marked) emit("available")
        }
    }

    // Every internet network, so the loss of the marked one is heard even
    // after it stopped being the default.
    private val allCallback = object : ConnectivityManager.NetworkCallback() {
        override fun onLost(network: Network) {
            if (network == marked) { marked = null; emit("lost") }
        }
    }

    init {
        EventChannel(engine.dartExecutor.binaryMessenger, "multitap/network")
            .setStreamHandler(object : EventChannel.StreamHandler {
                override fun onListen(arguments: Any?, events: EventChannel.EventSink) { sink = events; register() }
                override fun onCancel(arguments: Any?) { sink = null; unregister() }
            })
        MethodChannel(engine.dartExecutor.binaryMessenger, "multitap/network/mark").setMethodCallHandler { call, result ->
            if (call.method == "mark") { marked = defaultNetwork ?: manager?.activeNetwork; result.success(marked != null) }
            else result.notImplemented()
        }
    }

    private fun emit(event: String) {
        main.post { sink?.success(event) }
    }

    private fun register() {
        val connectivity = manager ?: return
        if (registered) return
        try {
            connectivity.registerDefaultNetworkCallback(defaultCallback)
            connectivity.registerNetworkCallback(
                NetworkRequest.Builder().addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET).build(),
                allCallback,
            )
            registered = true
        } catch (_: Exception) {
            // Without callbacks the session's own timeouts still notice a drop.
            unregister()
        }
    }

    private fun unregister() {
        val connectivity = manager ?: return
        try { connectivity.unregisterNetworkCallback(defaultCallback) } catch (_: Exception) {}
        try { connectivity.unregisterNetworkCallback(allCallback) } catch (_: Exception) {}
        registered = false
    }

    fun dispose() { sink = null; unregister() }
}
