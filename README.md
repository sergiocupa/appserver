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
- [ ] Study and subsequent implementation of an H.264 and H.265 video fragmenter. Fragments to be dispatched with a connection using the above transport protocol.
    - 🧩 Implementing an MP4-to-stream converter for use with the DASH protocol. Initially, I will run tests using the DASH protocol (HTTP-based streaming) to simplify the debugging process of the video chunks.
    - [ ] Take the above implementation and make it available for use by the operating system as a video source, such as a camera.
    - [ ] Integrate the stream preparer for use in real-time streaming, for UDP or WebSocket.
- [ ] Implementation of the protocol component in Javascript.
- [ ] Protocol tests.

- [ ] Javascript to manage the arrival buffer of video parts on the client side.
- [ ] Testing the video stream with browser. Test object synchronization with video stream.
- [ ] LINUX platform.
- [ ] Optimization and simplification.
- [ ] SSL support for HTTPs.
