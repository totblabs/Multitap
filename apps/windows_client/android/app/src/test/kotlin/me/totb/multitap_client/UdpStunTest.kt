package me.totb.multitap_client
import java.net.InetAddress
import java.net.InetSocketAddress
import org.junit.Assert.*
import org.junit.Test

class UdpStunTest {
    @Test fun mappedAddressRequiresSourceTransactionAndExactFraming(){
        val stun=UdpStun();val server=InetSocketAddress(InetAddress.getByAddress(byteArrayOf(127,0,0,1)),3478);stun.server(server)
        var request=ByteArray(0);stun.tick(1000){_,bytes->request=bytes.copyOf()}
        assertEquals(20,request.size)
        val response=request.copyOf(32);response[0]=1;response[1]=1;response[3]=12
        byteArrayOf(0,0x20,0,8,0,1,0x11,0x2b,0xe1.toByte(),0x12,0xa6.toByte(),0x43).copyInto(response,20)
        assertNull(stun.received(response,response.size,InetSocketAddress(server.address,3479),2000))
        val wrong=response.copyOf();wrong[8]=(wrong[8].toInt() xor 1).toByte();assertNull(stun.received(wrong,wrong.size,server,2000))
        assertNull(stun.received(response,response.size-1,server,2000))
        val mapped=stun.received(response,response.size,server,2000)!!
        assertEquals("192.0.2.1",mapped.address.hostAddress);assertEquals(12345,mapped.port)
        assertNull(stun.received(response,response.size,server,3000))
    }
    @Test fun serverCountAndRetriesAreBounded(){
        val stun=UdpStun();for(n in 1..10)stun.server(InetSocketAddress(InetAddress.getByAddress(byteArrayOf(127,0,0,n.toByte())),3478))
        var sent=0;for(now in 0L..9000000L step 100000L)stun.tick(now){_,_->sent++};assertEquals(10,sent)
    }
}
