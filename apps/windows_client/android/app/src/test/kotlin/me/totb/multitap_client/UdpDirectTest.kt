package me.totb.multitap_client

import java.net.InetAddress
import java.net.InetSocketAddress
import org.junit.Assert.*
import org.junit.Test

class UdpDirectTest {
    @Test fun nodeAndWindowsHmacReference(){
        val hex="5456440101000102030405060708090a0b0c0d0e0f3bee1b52544d6b8f4b01d4787080ebb46fbafa3aad919634be540a5d6b70b620"
        val packet=hex.chunked(2).map { it.toInt(16).toByte() }.toByteArray()
        val b=UdpDirect();b.configure(keys(2,1));var answered=false
        assertTrue(b.received(packet,packet.size,endpoint(1,1001),1000){_,_->answered=true});assertTrue(answered);b.wipe()
    }
    // Ordinary private addresses: loopback is probed only beside a loopback relay.
    private fun endpoint(last: Int, port: Int) = InetSocketAddress(InetAddress.getByAddress(byteArrayOf(10,0,0,last.toByte())),port)
    private fun at(a: Int, b: Int, c: Int, d: Int, port: Int = 5000) =
        InetSocketAddress(InetAddress.getByAddress(byteArrayOf(a.toByte(),b.toByte(),c.toByte(),d.toByte())),port)
    @Test fun targetsAreOrdinaryUnicastOnly() {
        val path=UdpDirect();path.configure(keys(1,2));path.relay(at(203,0,113,9,18481))
        for(bad in listOf(at(0,0,0,0),at(0,1,2,3),at(127,0,0,1),at(224,0,0,1),at(239,255,255,250),at(240,0,0,1),at(255,255,255,255))){
            assertFalse(path.routable(bad));path.candidate(bad)
        }
        assertEquals(0,path.candidates());assertFalse(path.routable(at(10,0,0,5,0)))
        assertTrue(path.routable(at(10,0,0,5))&&path.routable(at(198,51,100,7)))
        val local=UdpDirect();local.configure(keys(1,2));local.relay(at(127,0,0,1,18481));assertTrue(local.routable(at(127,0,0,1)))
        path.wipe();local.wipe()
    }
    @Test fun replayedPingsNeitherAnswerNorFillSlots() {
        val a=UdpDirect();val b=UdpDirect();a.configure(keys(1,2));b.configure(keys(2,1))
        val ea=endpoint(1,1001);val eb=endpoint(2,1002);a.candidate(eb);b.candidate(ea)
        var captured=ByteArray(0);val start=1000L;a.tick(start){to,bytes->if(to==eb)captured=bytes}
        var pongs=0
        assertTrue(b.received(captured,captured.size,ea,start){_,_->pongs++});assertEquals(1,pongs)
        assertTrue(b.received(captured,captured.size,ea,start+250000){_,_->pongs++});assertEquals(2,pongs)
        for(n in 1..6)assertTrue(b.received(captured,captured.size,at(198,51,100,n,40000),start+300000){_,_->pongs++})
        assertEquals(2,pongs);assertEquals(1,b.candidates());assertEquals(0,b.learned())
        for(n in 1..4){val now=start+n*UdpDirect.ALIVE_US+1000;var fresh=ByteArray(0)
            a.tick(now){to,bytes->if(to==eb)fresh=bytes};assertFalse(fresh.contentEquals(captured))
            b.received(fresh,fresh.size,at(198,51,100,10+n,40000),now){_,_->}}
        assertEquals(2,b.learned());assertEquals(3,b.candidates())
        b.tick(start+4*UdpDirect.ALIVE_US+UdpDirect.LEARNED_TRIAL_US+1000){_,_->}
        assertEquals(0,b.learned());assertEquals(1,b.candidates())
        a.wipe();b.wipe()
    }
    private fun keys(a: Byte,b: Byte)=UdpSessionKeys(UdpDirectionKey(ByteArray(32){a},ByteArray(4)),UdpDirectionKey(ByteArray(32){b},ByteArray(4)))
    @Test fun challengeAuthenticationReplayAndFallback() {
        val a=UdpDirect();val b=UdpDirect();a.configure(keys(1,2));b.configure(keys(2,1))
        val ea=endpoint(1,1001);val eb=endpoint(2,1002);a.candidate(eb);b.candidate(ea)
        var ping=ByteArray(0);a.tick(1000){_,bytes->ping=bytes}
        assertNull(a.target(1000));var answered=false
        val tampered=ping.copyOf();tampered[tampered.lastIndex]=(tampered.last().toInt() xor 1).toByte()
        assertFalse(b.received(tampered,tampered.size,ea,2000){_,_->answered=true});assertFalse(answered)
        var pong=ByteArray(0);assertTrue(b.received(ping,ping.size,ea,2000){_,bytes->pong=bytes})
        assertNull(b.target(2000));assertTrue(a.received(pong,pong.size,eb,3000){_,_->})
        assertEquals(eb,a.target(3000));assertTrue(a.accepts(eb,3000))
        assertNull(a.target(2000000));a.received(pong,pong.size,eb,2000000){_,_->};assertNull(a.target(2000000))
        assertFalse(b.received(ping,ping.size-1,ea,2000){_,_->})
        a.wipe();b.wipe()
    }
    @Test fun slowerDirectKeepsRelayAndLostPongsReturnToRelay() {
        val a=UdpDirect();val b=UdpDirect();a.configure(keys(1,2));b.configure(keys(2,1))
        val ea=endpoint(1,1001);val eb=endpoint(2,1002);val relay=endpoint(3,18481)
        a.relay(relay);b.relay(relay);a.candidate(eb);b.candidate(ea)
        data class Flight(val toA: Boolean,val from: InetSocketAddress,val bytes: ByteArray,val due: Long)
        var queue=ArrayList<Flight>();var now=1000L;var directDelay=100000L;var directWorks=true
        fun send(fromA: Boolean,to: InetSocketAddress,bytes: ByteArray){if(to==relay)queue.add(Flight(!fromA,relay,bytes,now+10000))else if(directWorks)queue.add(Flight(!fromA,if(fromA)ea else eb,bytes,now+directDelay))}
        fun run(end: Long){while(now<end){a.tick(now){to,bytes->send(true,to,bytes)};b.tick(now){to,bytes->send(false,to,bytes)};val held=queue;queue=ArrayList();for(f in held){if(f.due>now)queue.add(f)else (if(f.toA)a else b).received(f.bytes,f.bytes.size,f.from,now){to,bytes->send(f.toA,to,bytes)}};now+=1000}}
        run(1000000);assertNull(a.target(now));assertTrue(a.relayRtt()>0)
        directDelay=2000;run(2000000);assertEquals(eb,a.target(now));assertEquals(ea,b.target(now))
        directWorks=false;run(4000000);assertNull(a.target(now));assertNull(b.target(now))
        a.wipe();b.wipe()
    }
}
