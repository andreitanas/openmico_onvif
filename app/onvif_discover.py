import socket
import ustruct as struct
import os
import re

DEBUG = False

# Multicast address and port for WS-Discovery
MCAST_GRP = '239.255.255.250'
MCAST_PORT = 3702

# ONVIF Probe match response template
PROBE_MATCH_RESPONSE = """<?xml version="1.0" encoding="UTF-8"?>
<soap:Envelope xmlns:soap="http://www.w3.org/2003/05/soap-envelope"
               xmlns:wsa="http://schemas.xmlsoap.org/ws/2004/08/addressing"
               xmlns:d="http://schemas.xmlsoap.org/ws/2005/04/discovery">
  <soap:Header>
    <wsa:MessageID>urn:uuid:12345678-1234-1234-1234-123456789012</wsa:MessageID>
    <wsa:RelatesTo>{relates_to}</wsa:RelatesTo>
    <wsa:To>http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</wsa:To>
    <wsa:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/ProbeMatches</wsa:Action>
  </soap:Header>
  <soap:Body>
    <d:ProbeMatches>
      <d:ProbeMatch>
        <wsa:EndpointReference>
          <wsa:Address>urn:uuid:cb4f89da-70f5-4dbe-8ee0-{mac_address}</wsa:Address>
        </wsa:EndpointReference>
        <d:Types>dn:NetworkVideoTransmitter tds:Device</d:Types>
        <d:Scopes>onvif://www.onvif.org/type/video_encoder onvif://www.onvif.org/type/audio_encoder onvif://www.onvif.org/hardware/IPC onvif://www.onvif.org/name/openmiko onvif://www.onvif.org/location/Unknown</d:Scopes>
        <d:XAddrs>http://{ip_address}:8000/onvif/device_service</d:XAddrs>
        <d:MetadataVersion>1</d:MetadataVersion>
      </d:ProbeMatch>
    </d:ProbeMatches>
  </soap:Body>
</soap:Envelope>
"""

def run_service():
    # Create UDP socket
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((MCAST_GRP, MCAST_PORT))
    
    # Join multicast group
    mreq = struct.pack("4sl", socket.inet_aton(MCAST_GRP), socket.INADDR_ANY)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)

    # Get the MAC address
    ifconfig_output = os.popen("/sbin/ifconfig wlan0").read()
    mac_address = re.search(r'HWaddr\s+([0-9A-Fa-f:]{17})', ifconfig_output).group(1).lower().replace(":", "")
    ip_address = ifconfig_output.split("\n")[1].split()[1].split(':')[1]
    if DEBUG:
      print("IP Address: " + ip_address)
      print("MAC Address: " + mac_address)
      print("Listening for ONVIF WS-Discovery probes...")
    
    while True:
        try:
          data, addr = sock.recvfrom(1024)
          data = data.decode('utf-8')
          if DEBUG:
            print(addr)
            print(data)
          if 'Probe' in data:
              if DEBUG:
                print("Received probe")

              # extract message id from the probe
              start_tag = "MessageID"
              start_index = data.find(start_tag) + len(start_tag)
              start_index = data.find(">", start_index) + 1
              end_index = data.find("</", start_index)

              message_id = data[start_index:end_index].strip()

              # Create a Probe Match Response
              response = PROBE_MATCH_RESPONSE.format(
                  relates_to=message_id,
                  ip_address=ip_address,
                  mac_address=mac_address
              )
              
              # Send the Probe Match response
              sock.sendto(response.encode('utf-8'), addr)

              if DEBUG:
                print(response)
                print("-------------------------------------------------------")
        except Exception as e:
          print(e)

if __name__ == "__main__":
    run_service()
