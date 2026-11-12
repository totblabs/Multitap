package me.totb.multitap_client

import java.net.InetAddress
import java.net.InetSocketAddress
import java.security.SecureRandom

/** RFC 8489 Binding, native/windows_video/udp_stun.hpp. No media or credentials. */
internal class UdpStun {
    private class Server(val endpoint: InetSocketAddress) {
        val request=ByteArray(20)
        var next=0L;var started=0L;var attempts=0;var done=false
    }
    private val servers=ArrayList<Server>()
    private val random=SecureRandom()
    fun server(endpoint: InetSocketAddress){if(servers.size<2&&servers.none { it.endpoint==endpoint })servers.add(Server(endpoint))}
    fun tick(now: Long,send:(InetSocketAddress,ByteArray)->Unit){for(row in servers){
        if(row.done||row.attempts>=5||now<row.next)continue
        if(row.attempts==0){row.request[1]=1;cookie.copyInto(row.request,4);val transaction=ByteArray(12);random.nextBytes(transaction);transaction.copyInto(row.request,8);row.started=now}
        row.next=now+500000;row.attempts++;send(row.endpoint,row.request)
    }}
    fun received(bytes: ByteArray,size: Int,from: InetSocketAddress,now: Long): InetSocketAddress? {
        if(size<20||size>1200||bytes[0]!=1.toByte()||bytes[1]!=1.toByte()||!bytes.copyOfRange(4,8).contentEquals(cookie))return null
        val length=u16(bytes,2);if(length%4!=0||length!=size-20)return null
        val row=servers.find { !it.done&&it.attempts>0&&it.endpoint==from&&now-it.started<5000000&&it.request.copyOfRange(8,20).contentEquals(bytes.copyOfRange(8,20)) }?:return null
        var mapped: InetSocketAddress?=null;var at=20
        while(at<size){
            if(size-at<4)return null;val type=u16(bytes,at);val count=u16(bytes,at+2);at+=4
            if(count>size-at)return null
            if(type==0x20){
                if(mapped!=null||count!=8||bytes[at]!=0.toByte()||bytes[at+1]!=1.toByte())return null
                val port=u16(bytes,at+2) xor 0x2112;val address=ByteArray(4){(bytes[at+4+it].toInt() xor cookie[it].toInt()).toByte()}
                if(port==0||address.all { it==0.toByte() })return null
                mapped=InetSocketAddress(InetAddress.getByAddress(address),port)
            }else if(type<0x8000&&type!=1)return null
            val padded=(count+3) and 3.inv();if(padded>size-at)return null;at+=padded
        }
        if(mapped!=null)row.done=true
        return mapped
    }
    companion object {
        private val cookie=byteArrayOf(0x21,0x12,0xa4.toByte(),0x42)
        private fun u16(bytes: ByteArray,at: Int)=((bytes[at].toInt() and 255) shl 8) or (bytes[at+1].toInt() and 255)
    }
}
