# AppServer
HTTP, Application and Video Stream server. Can also be used as a WebAPI.
For use in embedded systems, but can also be used as a micro WebAPI.
At the moment, 07/11/2025, the implementation is not complete.

Features to be implemented:

- [X] Finalize parser and tests.
- [X] Method mapping, to forward actions to requests.
- [X] Create method mapping tests.
- [X] Download resources and HTML pages for presentation in browser.
- [X] Browser tests.
- [X] Handshake for websocket protocol.
- [X] Server-side multiplexing.
- [X] Define header and format for data transport. The protocol incorporates: stream-level multiplexing; application-level multiplexing; session scope control; stream flow control (timeline); FEC correction methods; stream-synchronized objects; it can be used as an object-only transport, in bidirectional mode; asynchronous mode at the transport level, not just in application mode (it does not wait for a response, as is the case with the HTTP protocol); secure mode waits for ACK (Action event, with or without Callback). The current version uses WebSocket for browser use and UDP for applications. For future versions, I will evaluate the use of the QUIC protocol.
- [ ] Implement an MP4 handler for preparing video fragments, with support for the H.264 and H.265 codecs. Initially, I will use a desktop window to facilitate the first stage of development.
    
    **Basic implementation**
    - [X] Extract MP4 metadata.
    - [X] Extract the frame index.
    - [X] Prepare the header for decoder initialization (Annex B SPS/PPS).
    - [X] Implement header and payload handling for H.264.
    - [X] Implement a desktop presentation window for testing, including screen resizing.
    - [X] Perform basic tests with video playback.
    
    **DASH server implementation (Adaptive streaming support will not be implemented, as this version is only for testing)**
    - [X] Create frame grouping ([moof][mdat]).
    - [X] Create an MP4 file with the initialization header.
    - [X] Create an MP4 file containing video fragment.
    - [X] Implement handshake between client and HTTP server.
    - [X] Implement an HTML page for browser-based testing.
    - [X] Perform browser tests.
       <br/>
       <img width="400" height="300" alt="image" src="https://github.com/user-attachments/assets/914032f7-2b74-4ccc-b4f6-48d3729e6578"/>
    - 🧩 Implementação para uso do decoder H265. Codec Wrapper para generalizar o suporte a H.264 e H.265.
    - [ ] Revisão do código. Bibliotecas auxiliares externas implementadas precisam ser revisadas e refatoradas.
    
    **MP4 creator**
    - [ ] Allow creation of a single file, header-only file, or fragment-only files.
    - [ ] Allow video creation from raw RGB or YUV frame input.
    
    **Compatibility**
    - [ ] Make all features compatible with the H.265 codec.
    - [ ] Test with videos extracted from YouTube, online samples, and an external camera.
    - [ ] After completing the implementations and tests, transfer the fragmenter to https://github.com/sergiocupa/MediaFragmenter
- [ ] Integrate the stream preparer for use in real-time streaming, for UDP or WebSocket.
- [ ] Implementation of the protocol component in Javascript.
- [ ] Protocol tests.

- [ ] Javascript to manage the arrival buffer of video parts on the client side.
- [ ] Testing the video stream with browser. Test object synchronization with video stream.
- [ ] LINUX platform.
- [ ] Optimization and simplification.
- [ ] SSL support for HTTPs.
