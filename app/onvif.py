import socket
import time
import os
import re

DEBUG = False

ONVIF_PORT = 8000

ONVIF_GET_DEVICE_INFORMATION_RESPONSE = """<?xml version="1.0" encoding="UTF-8"?>
<v:Envelope xmlns:v="http://www.w3.org/2003/05/soap-envelope" xmlns:wsa="http://schemas.xmlsoap.org/ws/2004/08/addressing">
    <v:Header>
        <wsa:To v:mustUnderstand="true">http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</wsa:To>
        <wsa:Action v:mustUnderstand="true">http://www.onvif.org/ver10/device/wsdl/GetDeviceInformation</wsa:Action>
    </v:Header>
    <v:Body>
        <GetDeviceInformationResponse xmlns="http://www.onvif.org/ver10/device/wsdl">
            <Manufacturer>Wyze</Manufacturer>
            <Model>openmiko</Model>
            <FirmwareVersion>0.001</FirmwareVersion>
            <SerialNumber>{mac_address}</SerialNumber>
            <HardwareId>{mac_address}</HardwareId>
        </GetDeviceInformationResponse>
    </v:Body>
</v:Envelope>
"""

ONVIF_GET_SYSTEM_DATE_AND_TIME_RESPONSE = """<?xml version="1.0" encoding="UTF-8"?>
<v:Envelope xmlns:v="http://www.w3.org/2003/05/soap-envelope" xmlns:wsa="http://schemas.xmlsoap.org/ws/2004/08/addressing" xmlns:d="http://www.onvif.org/ver10/device/wsdl">
    <v:Header>
        <wsa:To v:mustUnderstand="true">http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</wsa:To>
        <wsa:Action v:mustUnderstand="true">http://www.onvif.org/ver10/device/wsdl/GetSystemDateAndTime</wsa:Action>
    </v:Header>
    <v:Body>
        <d:GetSystemDateAndTimeResponse>
            <d:SystemDateAndTime>
                <d:DateTime>{date_time}</d:DateTime>
            </d:SystemDateAndTime>
        </d:GetSystemDateAndTimeResponse>
    </v:Body>
</v:Envelope>
"""

ONVIF_GET_ENDPOINT_REFERENCE_RESPONSE = """<?xml version="1.0" encoding="UTF-8"?>
<v:Envelope xmlns:v="http://www.w3.org/2003/05/soap-envelope" xmlns:wsa="http://schemas.xmlsoap.org/ws/2004/08/addressing" xmlns:d="http://www.onvif.org/ver10/device/wsdl">
    <v:Header>
        <wsa:To v:mustUnderstand="true">http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</wsa:To>
        <wsa:Action v:mustUnderstand="true">http://www.onvif.org/ver10/device/wsdl/GetEndpointReference</wsa:Action>
    </v:Header>
    <v:Body>
        <d:GetEndpointReferenceResponse>
            <d:EndpointReference>
                <d:Address>http://{ip_address}/onvif/device_service</d:Address>
            </d:EndpointReference>
        </d:GetEndpointReferenceResponse>
    </v:Body>
</v:Envelope>
"""

ONVIF_GET_CAPABILITIES_RESPONSE = """<?xml version="1.0" encoding="UTF-8"?>
<v:Envelope xmlns:v="http://www.w3.org/2003/05/soap-envelope" xmlns:wsa="http://schemas.xmlsoap.org/ws/2004/08/addressing" xmlns:tt="http://www.onvif.org/ver10/schema">
    <v:Header>
        <wsa:To v:mustUnderstand="true">http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</wsa:To>
        <wsa:Action v:mustUnderstand="true">http://www.onvif.org/ver10/device/wsdl/GetCapabilities</wsa:Action>
    </v:Header>
    <v:Body>
        <GetCapabilitiesResponse xmlns="http://www.onvif.org/ver10/device/wsdl">
            <Capabilities>
                <tt:Device>
                    <tt:XAddr>http://{ip_address}/onvif/device_service</tt:XAddr>
                </tt:Device>
                <tt:Media>
                    <tt:XAddr>http://{ip_address}/onvif/media_service</tt:XAddr>
                    <tt:StreamingCapabilities>
                        <tt:RTPMulticast>false</tt:RTPMulticast>
                        <tt:RTP_TCP>true</tt:RTP_TCP>
                        <tt:RTP_RTSP_TCP>true</tt:RTP_RTSP_TCP>
                    </tt:StreamingCapabilities>
                </tt:Media>
            </Capabilities>
        </GetCapabilitiesResponse>
    </v:Body>
</v:Envelope>
"""

ONVIF_GET_SERVICES_RESPONSE = """<?xml version="1.0" encoding="UTF-8"?>
<v:Envelope xmlns:v="http://www.w3.org/2003/05/soap-envelope" xmlns:wsa="http://schemas.xmlsoap.org/ws/2004/08/addressing" xmlns:d="http://www.onvif.org/ver10/device/wsdl">
    <v:Header>
        <wsa:To v:mustUnderstand="true">http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</wsa:To>
        <wsa:Action v:mustUnderstand="true">http://www.onvif.org/ver10/device/wsdl/GetServices</wsa:Action>
    </v:Header>
    <v:Body>
        <d:GetServicesResponse>
            <d:Services>
                <d:Service>
                    <d:Namespace>http://www.onvif.org/ver10/device/wsdl</d:Namespace>
                    <d:XAddr>http://{ip_address}/onvif/device_service</d:XAddr>
                    <d:Version>
                        <d:Major>2</d:Major>
                        <d:Minor>5</d:Minor>
                    </d:Version>
                </d:Service>
                <d:Service>
                    <d:Namespace>http://www.onvif.org/ver10/media/wsdl</d:Namespace>
                    <d:XAddr>http://{ip_address}/onvif/media_service</d:XAddr>
                    <d:Version>
                        <d:Major>2</d:Major>
                        <d:Minor>5</d:Minor>
                    </d:Version>
                </d:Service>
                <d:Service>
                    <d:Namespace>http://www.onvif.org/ver20/media/wsdl</d:Namespace>
                    <d:XAddr>http://{ip_address}/onvif/media2_service</d:XAddr>
                    <d:Version>
                        <d:Major>2</d:Major>
                        <d:Minor>5</d:Minor>
                    </d:Version>
                </d:Service>
            </d:Services>
        </d:GetServicesResponse>
    </v:Body>
</v:Envelope>
"""

ONVIF_GET_SNAPSHOT_URI_RESPONSE = """<?xml version="1.0" encoding="UTF-8"?>
<v:Envelope xmlns:v="http://www.w3.org/2003/05/soap-envelope" xmlns:wsa="http://schemas.xmlsoap.org/ws/2004/08/addressing" xmlns:d="http://www.onvif.org/ver10/device/wsdl">
    <v:Header>
        <wsa:To v:mustUnderstand="true">http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</wsa:To>
        <wsa:Action v:mustUnderstand="true">http://www.onvif.org/ver10/device/wsdl/GetSnapshotUri</wsa:Action>
    </v:Header>
    <v:Body>
        <d:GetSnapshotUriResponse>
            <d:Uri>http://{ip_address}:8080/?action=snapshot</d:Uri>
        </d:GetSnapshotUriResponse>
    </v:Body>
</v:Envelope>
"""

ONVIF_GET_AUDIO_OUTPUT_CONFIGURATIONS_RESPONSE = """<?xml version="1.0" encoding="UTF-8"?>
<v:Envelope xmlns:v="http://www.w3.org/2003/05/soap-envelope" xmlns:wsa="http://schemas.xmlsoap.org/ws/2004/08/addressing" xmlns:d="http://www.onvif.org/ver10/device/wsdl">
    <v:Header>
        <wsa:To v:mustUnderstand="true">http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</wsa:To>
        <wsa:Action v:mustUnderstand="true">http://www.onvif.org/ver10/device/wsdl/GetAudioOutputConfigurations</wsa:Action>
    </v:Header>
    <v:Body>
        <d:GetAudioOutputConfigurationsResponse>
            <d:Configurations>
                <d:Configuration>
                    <d:Name>Default</d:Name>
                    <d:UseCount>1</d:UseCount>
                    <d:Token>AudioOutputConfig_1</d:Token>
                    <d:AudioOutputMode>Mono</d:AudioOutputMode>
                </d:Configuration>
            </d:Configurations>
        </d:GetAudioOutputConfigurationsResponse>
    </v:Body>
</v:Envelope>
"""

ONVIF_ADD_AUDIO_OUTPUT_CONFIGURATION_RESPONSE = """<?xml version="1.0" encoding="UTF-8"?>
<v:Envelope xmlns:v="http://www.w3.org/2003/05/soap-envelope" xmlns:wsa="http://schemas.xmlsoap.org/ws/2004/08/addressing" xmlns:d="http://www.onvif.org/ver10/device/wsdl">
    <v:Header>
        <wsa:To v:mustUnderstand="true">http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</wsa:To>
        <wsa:Action v:mustUnderstand="true">http://www.onvif.org/ver10/device/wsdl/AddAudioOutputConfiguration</wsa:Action>
    </v:Header>
    <v:Body>
        <d:AddAudioOutputConfigurationResponse>
            <d:ConfigurationToken>AudioOutputConfig_1</d:ConfigurationToken>
        </d:AddAudioOutputConfigurationResponse>
    </v:Body>
</v:Envelope>
"""

ONVIF_GET_NETWORK_PROTOCOLS_RESPONSE = """<?xml version="1.0" encoding="UTF-8"?>
<v:Envelope xmlns:v="http://www.w3.org/2003/05/soap-envelope" xmlns:wsa="http://schemas.xmlsoap.org/ws/2004/08/addressing" xmlns:d="http://www.onvif.org/ver10/device/wsdl">
    <v:Header>
        <wsa:To v:mustUnderstand="true">http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</wsa:To>
        <wsa:Action v:mustUnderstand="true">http://www.onvif.org/ver10/device/wsdl/GetNetworkProtocols</wsa:Action>
    </v:Header>
    <v:Body>
        <d:GetNetworkProtocolsResponse>
            <d:NetworkProtocols>
                <d:NetworkProtocol>
                    <d:Name>HTTP</d:Name>
                    <d:UseCount>1</d:UseCount>
                    <d:Token>NetworkProtocol_1</d:Token>
                    <d:Enabled>true</d:Enabled>
                </d:NetworkProtocol>
            </d:NetworkProtocols>
        </d:GetNetworkProtocolsResponse>
    </v:Body>
</v:Envelope>
"""

ONVIF_CREATE_PROFILE_RESPONSE = """<?xml version="1.0" encoding="UTF-8"?>
<v:Envelope xmlns:v="http://www.w3.org/2003/05/soap-envelope" xmlns:wsa="http://schemas.xmlsoap.org/ws/2004/08/addressing" xmlns:d="http://www.onvif.org/ver10/device/wsdl">
    <v:Header>
        <wsa:To v:mustUnderstand="true">http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</wsa:To>
        <wsa:Action v:mustUnderstand="true">http://www.onvif.org/ver10/device/wsdl/CreateProfile</wsa:Action>
    </v:Header>
    <v:Body>
        <d:CreateProfileResponse>
            <d:ProfileToken>Profile_1</d:ProfileToken>
        </d:CreateProfileResponse>
    </v:Body>
</v:Envelope>
"""

ONVIF_DELETE_PROFILE_RESPONSE = """<?xml version="1.0" encoding="UTF-8"?>
<v:Envelope xmlns:v="http://www.w3.org/2003/05/soap-envelope" xmlns:wsa="http://schemas.xmlsoap.org/ws/2004/08/addressing" xmlns:d="http://www.onvif.org/ver10/device/wsdl">
    <v:Header>
        <wsa:To v:mustUnderstand="true">http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</wsa:To>
        <wsa:Action v:mustUnderstand="true">http://www.onvif.org/ver10/device/wsdl/DeleteProfile</wsa:Action>
    </v:Header>
    <v:Body>
        <d:DeleteProfileResponse/>
    </v:Body>
</v:Envelope>
"""

ONVIF_GET_PROFILE_RESPONSE = """<?xml version="1.0" encoding="UTF-8"?>
<v:Envelope xmlns:v="http://www.w3.org/2003/05/soap-envelope" xmlns:wsa="http://schemas.xmlsoap.org/ws/2004/08/addressing" xmlns:tt="http://www.onvif.org/ver10/schema" xmlns:trt="http://www.onvif.org/ver10/media/wsdl">
    <v:Header>
        <wsa:To v:mustUnderstand="true">http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</wsa:To>
        <wsa:Action v:mustUnderstand="true">http://www.onvif.org/ver10/device/wsdl/GetProfile</wsa:Action>
    </v:Header>
    <v:Body>
        <trt:GetProfileResponse>
            <trt:Profile fixed="true" token="Profile_1">
                <tt:Name>MainStream</tt:Name>
                <tt:VideoSourceConfiguration token="VideoSourceConfig_1">
                    <tt:Name>VideoSourceConfig</tt:Name>
                    <tt:UseCount>1</tt:UseCount>
                    <tt:SourceToken>VideoSource_1</tt:SourceToken>
                    <tt:Bounds height="1080" width="1920" y="0" x="0"/>
                </tt:VideoSourceConfiguration>
                <tt:VideoEncoderConfiguration token="VideoEncoderConfig_1">
                    <tt:Name>VideoEncoderConfig</tt:Name>
                    <tt:UseCount>1</tt:UseCount>
                    <tt:Encoding>H264</tt:Encoding>
                    <tt:Resolution>
                        <tt:Width>1920</tt:Width>
                        <tt:Height>1080</tt:Height>
                    </tt:Resolution>
                    <tt:Quality>5</tt:Quality>
                    <tt:RateControl>
                        <tt:FrameRateLimit>30</tt:FrameRateLimit>
                        <tt:EncodingInterval>1</tt:EncodingInterval>
                        <tt:BitrateLimit>4096</tt:BitrateLimit>
                    </tt:RateControl>
                    <tt:H264>
                        <tt:GovLength>25</tt:GovLength>
                        <tt:H264Profile>High</tt:H264Profile>
                    </tt:H264>
                </tt:VideoEncoderConfiguration>
            </trt:Profile>
        </trt:GetProfileResponse>
    </v:Body>
</v:Envelope>
"""


ONVIF_GET_PROFILES_RESPONSE = """<?xml version="1.0" encoding="UTF-8"?>
<v:Envelope xmlns:v="http://www.w3.org/2003/05/soap-envelope" xmlns:wsa="http://schemas.xmlsoap.org/ws/2004/08/addressing" xmlns:tt="http://www.onvif.org/ver10/schema" xmlns:trt="http://www.onvif.org/ver10/media/wsdl">
    <v:Header>
        <wsa:To v:mustUnderstand="true">http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</wsa:To>
        <wsa:Action v:mustUnderstand="true">http://www.onvif.org/ver10/device/wsdl/GetProfiles</wsa:Action>
    </v:Header>
    <v:Body>
        <trt:GetProfilesResponse>
            <trt:Profiles fixed="true" token="Profile_1">
                <tt:Name>MainStream</tt:Name>
                <tt:VideoSourceConfiguration token="VideoSourceConfig_1">
                    <tt:Name>VideoSourceConfig</tt:Name>
                    <tt:UseCount>1</tt:UseCount>
                    <tt:SourceToken>VideoSource_1</tt:SourceToken>
                    <tt:Bounds height="1080" width="1920" y="0" x="0"/>
                </tt:VideoSourceConfiguration>
                <tt:VideoEncoderConfiguration token="VideoEncoderConfig_1">
                    <tt:Name>VideoEncoderConfig</tt:Name>
                    <tt:UseCount>1</tt:UseCount>
                    <tt:Encoding>H264</tt:Encoding>
                    <tt:Resolution>
                        <tt:Width>1920</tt:Width>
                        <tt:Height>1080</tt:Height>
                    </tt:Resolution>
                    <tt:Quality>5</tt:Quality>
                    <tt:RateControl>
                        <tt:FrameRateLimit>30</tt:FrameRateLimit>
                        <tt:EncodingInterval>1</tt:EncodingInterval>
                        <tt:BitrateLimit>4096</tt:BitrateLimit>
                    </tt:RateControl>
                    <tt:H264>
                        <tt:GovLength>25</tt:GovLength>
                        <tt:H264Profile>High</tt:H264Profile>
                    </tt:H264>
                </tt:VideoEncoderConfiguration>
            </trt:Profiles>
        </trt:GetProfilesResponse>
    </v:Body>
</v:Envelope>
"""

ONVIF_GET_VIDEO_SOURCES_RESPONSE = """<?xml version="1.0" encoding="UTF-8"?>
<v:Envelope xmlns:v="http://www.w3.org/2003/05/soap-envelope" xmlns:wsa="http://schemas.xmlsoap.org/ws/2004/08/addressing" xmlns:d="http://www.onvif.org/ver10/device/wsdl">
    <v:Header>
        <wsa:To v:mustUnderstand="true">http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</wsa:To>
        <wsa:Action v:mustUnderstand="true">http://www.onvif.org/ver10/device/wsdl/GetVideoSources</wsa:Action>
    </v:Header>
    <v:Body>
        <d:GetVideoSourcesResponse>
            <d:VideoSources>
                <d:VideoSource>
                    <d:Name>Main</d:Name>
                    <d:UseCount>1</d:UseCount>
                    <d:SourceToken>VideoSource_1</d:SourceToken>
                </d:VideoSource>
            </d:VideoSources>
        </d:GetVideoSourcesResponse>
    </v:Body>
</v:Envelope>
"""

ONVIF_GET_VIDEO_SOURCE_CONFIGURATIONS_RESPONSE = """<?xml version="1.0" encoding="UTF-8"?>
<v:Envelope xmlns:v="http://www.w3.org/2003/05/soap-envelope" xmlns:wsa="http://schemas.xmlsoap.org/ws/2004/08/addressing" xmlns:d="http://www.onvif.org/ver10/device/wsdl">
    <v:Header>
        <wsa:To v:mustUnderstand="true">http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</wsa:To>
        <wsa:Action v:mustUnderstand="true">http://www.onvif.org/ver10/device/wsdl/GetVideoSourceConfigurations</wsa:Action>
    </v:Header>
    <v:Body>
        <d:GetVideoSourceConfigurationsResponse>
            <d:Configurations>
                <d:Configuration>
                    <d:Name>Main</d:Name>
                    <d:UseCount>1</d:UseCount>
                    <d:Token>VideoSourceConfig_1</d:Token>
                </d:Configuration>
            </d:Configurations>
        </d:GetVideoSourceConfigurationsResponse>
    </v:Body>
</v:Envelope>
"""

ONVIF_GET_VIDEO_ENCODER_CONFIGURATIONS_RESPONSE = """<?xml version="1.0" encoding="UTF-8"?>
<v:Envelope xmlns:v="http://www.w3.org/2003/05/soap-envelope" xmlns:wsa="http://schemas.xmlsoap.org/ws/2004/08/addressing" xmlns:d="http://www.onvif.org/ver10/device/wsdl">
    <v:Header>
        <wsa:To v:mustUnderstand="true">http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</wsa:To>
        <wsa:Action v:mustUnderstand="true">http://www.onvif.org/ver10/device/wsdl/GetVideoEncoderConfigurations</wsa:Action>
    </v:Header>
    <v:Body>
        <d:GetVideoEncoderConfigurationsResponse>
            <d:Configurations>
                <d:Configuration>
                    <d:Name>Main</d:Name>
                    <d:UseCount>1</d:UseCount>
                    <d:Token>VideoEncoderConfig_1</d:Token>
                </d:Configuration>
            </d:Configurations>
        </d:GetVideoEncoderConfigurationsResponse>
    </v:Body>
</v:Envelope>
"""

ONVIF_GET_AUDIO_SOURCES_RESPONSE = """<?xml version="1.0" encoding="UTF-8"?>
<v:Envelope xmlns:v="http://www.w3.org/2003/05/soap-envelope" xmlns:wsa="http://schemas.xmlsoap.org/ws/2004/08/addressing" xmlns:d="http://www.onvif.org/ver10/device/wsdl">
    <v:Header>
        <wsa:To v:mustUnderstand="true">http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</wsa:To>
        <wsa:Action v:mustUnderstand="true">http://www.onvif.org/ver10/device/wsdl/GetAudioSources</wsa:Action>
    </v:Header>
    <v:Body>
        <d:GetAudioSourcesResponse>
            <d:AudioSources>
                <d:AudioSource>
                    <d:Name>Main</d:Name>
                    <d:UseCount>1</d:UseCount>
                    <d:SourceToken>AudioSource_1</d:SourceToken>
                </d:AudioSource>
            </d:AudioSources>
        </d:GetAudioSourcesResponse>
    </v:Body>
</v:Envelope>
"""

ONVIF_GET_AUDIO_SOURCE_CONFIGURATIONS_RESPONSE = """<?xml version="1.0" encoding="UTF-8"?>
<v:Envelope xmlns:v="http://www.w3.org/2003/05/soap-envelope" xmlns:wsa="http://schemas.xmlsoap.org/ws/2004/08/addressing" xmlns:d="http://www.onvif.org/ver10/device/wsdl">
    <v:Header>
        <wsa:To v:mustUnderstand="true">http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</wsa:To>
        <wsa:Action v:mustUnderstand="true">http://www.onvif.org/ver10/device/wsdl/GetAudioSourceConfigurations</wsa:Action>
    </v:Header>
    <v:Body>
        <d:GetAudioSourceConfigurationsResponse>
            <d:Configurations>
                <d:Configuration>
                    <d:Name>Main</d:Name>
                    <d:UseCount>1</d:UseCount>
                    <d:Token>AudioSourceConfig_1</d:Token>
                </d:Configuration>
            </d:Configurations>
        </d:GetAudioSourceConfigurationsResponse>
    </v:Body>
</v:Envelope>
"""

ONVIF_GET_AUDIO_ENCODER_CONFIGURATIONS_RESPONSE = """<?xml version="1.0" encoding="UTF-8"?>
<v:Envelope xmlns:v="http://www.w3.org/2003/05/soap-envelope" xmlns:wsa="http://schemas.xmlsoap.org/ws/2004/08/addressing" xmlns:d="http://www.onvif.org/ver10/device/wsdl">
    <v:Header>
        <wsa:To v:mustUnderstand="true">http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</wsa:To>
        <wsa:Action v:mustUnderstand="true">http://www.onvif.org/ver10/device/wsdl/GetAudioEncoderConfigurations</wsa:Action>
    </v:Header>
    <v:Body>
        <d:GetAudioEncoderConfigurationsResponse>
            <d:Configurations>
                <d:Configuration>
                    <d:Name>Main</d:Name>
                    <d:UseCount>1</d:UseCount>
                    <d:Token>AudioEncoderConfig_1</d:Token>
                </d:Configuration>
            </d:Configurations>
        </d:GetAudioEncoderConfigurationsResponse>
    </v:Body>
</v:Envelope>
"""

ONVIF_GET_STREAM_URI_RESPONSE = """<?xml version="1.0" encoding="UTF-8"?>
<v:Envelope xmlns:v="http://www.w3.org/2003/05/soap-envelope" xmlns:wsa="http://schemas.xmlsoap.org/ws/2004/08/addressing" xmlns:trt="http://www.onvif.org/ver10/media/wsdl">
    <v:Header>
        <wsa:To v:mustUnderstand="true">http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</wsa:To>
        <wsa:Action v:mustUnderstand="true">http://www.onvif.org/ver10/media/wsdl/GetStreamUri</wsa:Action>
    </v:Header>
    <v:Body>
        <trt:GetStreamUriResponse>
            <trt:MediaUri>
                <tt:Uri>{rtsp_url}</tt:Uri>
                <tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>
                <tt:InvalidAfterReboot>false</tt:InvalidAfterReboot>
                <tt:Timeout>PT0S</tt:Timeout>
            </trt:MediaUri>
        </trt:GetStreamUriResponse>
    </v:Body>
</v:Envelope>
"""

ONVIF_SET_SYNCHRONIZATION_POINT_RESPONSE = """<?xml version="1.0" encoding="UTF-8"?>
<v:Envelope xmlns:v="http://www.w3.org/2003/05/soap-envelope" xmlns:wsa="http://schemas.xmlsoap.org/ws/2004/08/addressing" xmlns:d="http://www.onvif.org/ver10/device/wsdl">
    <v:Header>
        <wsa:To v:mustUnderstand="true">http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</wsa:To>
        <wsa:Action v:mustUnderstand="true">http://www.onvif.org/ver10/device/wsdl/SetSynchronizationPoint</wsa:Action>
    </v:Header>
    <v:Body>
        <d:SetSynchronizationPointResponse/>
    </v:Body>
</v:Envelope>
"""

def send_message(conn, message):
    pos = 0
    while pos < len(message):
        pos += conn.send(message[pos:])

def run_service():
    # Create a UDP socket for ONVIF discovery
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(('0.0.0.0', ONVIF_PORT))
    sock.listen(1)
    
    # Get the MAC address
    ifconfig_output = os.popen("/sbin/ifconfig wlan0").read()
    mac_address = re.search(r'HWaddr\s+([0-9A-Fa-f:]{17})', ifconfig_output).group(1).lower().replace(":", "")

    if DEBUG:
        print("MAC Address: " + mac_address)
        print("ONVIF server started on port " + str(ONVIF_PORT))

    while True:
        try:
            conn, addr = sock.accept()
            if DEBUG:
                print("Received connection from " + addr[0] + ":" + str(addr[1]))
            data = conn.recv(4096).decode("utf-8")
            if DEBUG:
                print(data)
            # parse host from Host header
            host = data.split("Host: ")[1].split("\r\n")[0]
            # get current ip address
            ip_address = host.split(":")[0]
            if DEBUG:
                print("Received data from " + addr[0] + ":" + str(addr[1]) + " for " + ip_address)

            # Prepare HTTP response headers
            http_response_head = "HTTP/1.1 200 OK\r\n"
            http_response_head += "Content-Type: application/soap+xml; charset=utf-8\r\n"
            http_response_head += "Server: Python/ONVIF\r\n"
            http_response_head += "Connection: close\r\n"
            
            if 'GetDeviceInformation' in data:
                http_response_body = ONVIF_GET_DEVICE_INFORMATION_RESPONSE.replace("{mac_address}", mac_address)
                if DEBUG:
                    print("Sent ONVIF GetDeviceInformation response")
            elif 'GetSystemDateAndTime' in data:
                t = time.localtime()
                http_response_body = ONVIF_GET_SYSTEM_DATE_AND_TIME_RESPONSE.replace(
                    "{date_time}",
                    "{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}".format(
                        t.tm_year, t.tm_mon, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec))
                if DEBUG:
                    print("Sent ONVIF GetSystemDateAndTime response")
            elif 'GetEndpointReference' in data:
                http_response_body = ONVIF_GET_ENDPOINT_REFERENCE_RESPONSE
                if DEBUG:
                    print("Sent ONVIF GetEndpointReference response")
            elif 'GetCapabilities' in data:
                http_response_body = ONVIF_GET_CAPABILITIES_RESPONSE
                if DEBUG:
                    print("Sent ONVIF GetCapabilities response")
            elif 'GetServices' in data:
                http_response_body = ONVIF_GET_SERVICES_RESPONSE
                if DEBUG:
                    print("Sent ONVIF GetServices response")
            elif 'GetSnapshotUri' in data:
                http_response_body = ONVIF_GET_SNAPSHOT_URI_RESPONSE
                if DEBUG:
                    print("Sent ONVIF GetSnapshotUri response")
            elif 'GetAudioOutputConfigurations' in data:
                http_response_body = ONVIF_GET_AUDIO_OUTPUT_CONFIGURATIONS_RESPONSE
                if DEBUG:
                    print("Sent ONVIF GetAudioOutputConfigurations response")
            elif 'AddAudioOutputConfiguration' in data:
                http_response_body = ONVIF_ADD_AUDIO_OUTPUT_CONFIGURATION_RESPONSE
                if DEBUG:
                    print("Sent ONVIF AddAudioOutputConfiguration response")
            elif 'GetNetworkProtocols' in data:
                http_response_body = ONVIF_GET_NETWORK_PROTOCOLS_RESPONSE
                if DEBUG:
                    print("Sent ONVIF GetNetworkProtocols response")
            elif 'CreateProfile' in data:
                http_response_body = ONVIF_CREATE_PROFILE_RESPONSE
                if DEBUG:
                    print("Sent ONVIF CreateProfile response")
            elif 'DeleteProfile' in data:
                http_response_body = ONVIF_DELETE_PROFILE_RESPONSE
                if DEBUG:
                    print("Sent ONVIF DeleteProfile response")
            elif 'GetProfiles' in data:
                http_response_body = ONVIF_GET_PROFILES_RESPONSE
                if DEBUG:
                    print("Sent ONVIF GetProfiles response")
            elif 'GetProfile' in data:
                http_response_body = ONVIF_GET_PROFILE_RESPONSE
                if DEBUG:
                    print("Sent ONVIF GetProfile response")
            elif 'GetVideoSources' in data:
                http_response_body = ONVIF_GET_VIDEO_SOURCES_RESPONSE
                if DEBUG:
                    print("Sent ONVIF GetVideoSources response")
            elif 'GetVideoSourceConfigurations' in data:
                http_response_body = ONVIF_GET_VIDEO_SOURCE_CONFIGURATIONS_RESPONSE
                if DEBUG:
                    print("Sent ONVIF GetVideoSourceConfigurations response")
            elif 'GetVideoEncoderConfigurations' in data:
                http_response_body = ONVIF_GET_VIDEO_ENCODER_CONFIGURATIONS_RESPONSE
                if DEBUG:
                    print("Sent ONVIF GetVideoEncoderConfigurations response")
            elif 'GetAudioSources' in data:
                http_response_body = ONVIF_GET_AUDIO_SOURCES_RESPONSE
                if DEBUG:
                    print("Sent ONVIF GetAudioSources response")
            elif 'GetAudioSourceConfigurations' in data:
                http_response_body = ONVIF_GET_AUDIO_SOURCE_CONFIGURATIONS_RESPONSE
                if DEBUG:
                    print("Sent ONVIF GetAudioSourceConfigurations response")
            elif 'GetAudioEncoderConfigurations' in data:
                http_response_body = ONVIF_GET_AUDIO_ENCODER_CONFIGURATIONS_RESPONSE
                if DEBUG:
                    print("Sent ONVIF GetAudioEncoderConfigurations response")
            elif 'GetStreamUri' in data:
                rtsp_url = "rtsp://{ip_address}:8554/video3_unicast".replace("{ip_address}", ip_address)
                http_response_body = ONVIF_GET_STREAM_URI_RESPONSE.replace("{rtsp_url}", rtsp_url)
                if DEBUG:
                    print("Sent ONVIF GetStreamUri response")
            elif 'SetSynchronizationPoint' in data:
                http_response_body = ONVIF_SET_SYNCHRONIZATION_POINT_RESPONSE
                if DEBUG:
                    print("Sent ONVIF SetSynchronizationPoint response")
            else:
                print("Unsupported request: " + data)
                http_response_head = "HTTP/1.1 400 Bad Request\r\n"
                http_response_head += "Content-Type: text/html\r\n"
                http_response_head += "Connection: close\r\n"
                http_response_body = "<html><body><h1>400 Bad Request</h1></body></html>"

            http_response_body = http_response_body.replace("{ip_address}", ip_address)
            now = time.gmtime()
            expires = time.gmtime(time.time() + 120)
            http_response_body = http_response_body.replace(
                "<v:Header>",
                """<v:Header>
                    <wsse:Security xmlns:wsse="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd" xmlns:wsu="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd">
                        <wsu:Timestamp wsu:Id="Timestamp">
                            <wsu:Created>{created}</wsu:Created>
                            <wsu:Expires>{expires}</wsu:Expires>
                        </wsu:Timestamp>
                    </wsse:Security>""".format(
                    created="{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}".format(
                        now.tm_year, now.tm_mon, now.tm_mday, now.tm_hour, now.tm_min, now.tm_sec),
                    expires="{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}".format(
                        expires.tm_year, expires.tm_mon, expires.tm_mday, expires.tm_hour, expires.tm_min, expires.tm_sec)
                )
            )
            encoded_body = http_response_body.encode('utf-8')
            content_length = len(encoded_body)
            http_response_head += "Content-Length: " + str(content_length) + "\r\n\r\n"
            send_message(conn, http_response_head.encode('utf-8'))
            send_message(conn, encoded_body)
            if DEBUG:
                print(http_response_head)
                print(http_response_body)
                print("------------------------------------------------------------------")

            conn.close()
        except Exception as e:
            print(e)

if __name__ == "__main__":
    run_service()
