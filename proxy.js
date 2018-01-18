var HOST_INTERFACE = '10.20.9.1';   // This controls the output interface for the multicasts

var INCOMING_MULTICAST = "234.18.1.1";      // The source multicast address
var INCOMING_PORT = "5000";                 //    and port number

var DESTINATION_MULTICAST = "234.18.1.2";   // The first multicast output address
var DESTINATION_PORT = "5000";              //    and port number

var DESTINATION_MULTICAST2 = "234.18.1.3";  // The second multicast output address
var DESTINATION_PORT2 = "5000";             //    and port number

var PERCENT_PACKET_LOSS = 0.2;    // The amount of packets that should be dropped on the output.

var verbose = false;               // dump out debug - which rtp sequence numbered packets are dropped on which output.


// -------------------------------------------------------------------------------------------------------------------
// End of config params
// -------------------------------------------------------------------------------------------------------------------

var dgram = require('dgram');

var incoming = [];
incoming.push(
    {
        socket:            dgram.createSocket({type: 'udp4', reuseAddr: true}),
        multicast_address: INCOMING_MULTICAST,
        multicast_port:    INCOMING_PORT,
        packetDropCount:   0
    }
);

var outgoing = []
outgoing.push(
    {
        socket:            dgram.createSocket({type: 'udp4', reuseAddr: true}),
        multicast_address: DESTINATION_MULTICAST,
        multicast_port:    DESTINATION_PORT,
        packetDropCount:   0
    }
);

outgoing.push(
    {
        socket:            dgram.createSocket({type: 'udp4', reuseAddr: true}),
        multicast_address: DESTINATION_MULTICAST2,
        multicast_port:    DESTINATION_PORT2,
        packetDropCount:   0
    }
);

outgoing.forEach(function(og){
    og.socket.bind(HOST_INTERFACE);
    og.socket.on('listening', function() {
        this.setBroadcast(true);
        this.setMulticastTTL(128);
    });
});

incoming.forEach(function(ig) {
    ig.socket.on('listening', function () {
        var addr = this.address();
        console.log('Client listening on ' + addr.address + ":" + addr.port);
        this.addMembership(ig.multicast_address, HOST_INTERFACE);
    });
});

var input_filter_pass  = () => (true);
var output_filter_pass = () => (Math.random() > (PERCENT_PACKET_LOSS / 100.0));

incoming[0].socket.on('message', function (incoming_packet, remote) {
    if (input_filter_pass()) {

        var PacketAlreadyDropped = false; // Use to prevent coincident packet loss.

        outgoing.forEach(function(og) {

            var packetDrop = false;
            if (!output_filter_pass()) {
                // Drop a packet, so increment count on output.
                og.packetDropCount += 1;
            }

            if (!PacketAlreadyDropped && og.packetDropCount)
            {
                // We haven't dropped this packet on any other output
                // but need to for this output then signal drop and
                // decrement the count.
                packetDrop = true;
                og.packetDropCount -= 1;
            }

            if (!packetDrop) {
                og.socket.send(incoming_packet,
                               0,
                               incoming_packet.length,
                               og.multicast_port,og.multicast_address);
            } else {
                PacketAlreadyDropped = true;
                if (verbose) {
                    let packetDropNum = (incoming_packet[2] << 8) + incoming_packet[3];
                    console.log("Output(" + og.multicast_address + ") Dropping RTP# " + packetDropNum);
                }
            }

        });
    } else {
        if (verbose) {
            let packetDropNum = (incoming_packet[2] << 8) + incoming_packet[3];
            console.log("Input (" + ig.multicast_address + ") Dropping RTP# " + packetDropNum);
        }
    }
});

incoming[0].socket.bind(incoming[0].multicast_port);