# AppServer
HTTP, Application and Video Stream server. Can also be used as a WebAPI.
For use in embedded systems, but can also be used as a micro WebAPI.
At the moment, 27/08/2025, the implementation is not complete.

Features to be implemented:

- [X] Finalize parser and tests.
- [X] Method mapping, to forward actions to requests.
- [X] Create method mapping tests.
- [X] Download resources and HTML pages for presentation in browser.
- [X] Browser tests.
- [X] Handshake for websocket protocol.
- [X] Server-side multiplexing.
- [X] Define header and format for data transport. The current version uses WebSocket for browser use, and UDP for apps. The protocol incorporates: stream-level multiplexing; application-level multiplexing; stream flow control (timeline); FEC correction methods; and stream-synchronized objects. For future versions, I will evaluate the use of the QUIC protocol.
- [ ] Implementation of the protocol component in Javascript.
- [ ] Create session scope control.
- [ ] Websocket tests.
- [ ] Study and subsequent implementation of an H.264 video fragmenter. Fragments to be dispatched using a routed websocket connection for presentation in browser.
- [ ] Javascript to manage the arrival buffer of video parts on the client side.
- [ ] Testing the video stream with browser.
- [ ] LINUX platform.
- [ ] Optimization and simplification.
- [ ] SSL support for HTTPs.
