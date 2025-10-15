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
- [X] Define header and format for data transport. The protocol incorporates: stream-level multiplexing; application-level multiplexing; session scope control; stream flow control (timeline); FEC correction methods; stream-synchronized objects; it can be used as an object-only transport, in bidirectional mode; asynchronous mode at the transport level, not just in application mode (it does not wait for a response, as is the case with the HTTP protocol); secure mode waits for ACK (Action event, with or without Callback). The current version uses WebSocket for browser use and UDP for applications. For future versions, I will evaluate the use of the QUIC protocol.
- [ ] Study and subsequent implementation of an H.264 and H.265 video fragmenter. Fragments to be dispatched with a connection using the above transport protocol.
    - 🧩 Implementando conversor de arquivo mp4 para stream para uso em protocolo Dash. Vou testar primeiro usando o protocolo dash (fluxo por http), para facilitar esta etapa de depuração dos chunks de stream de video. Após esta etapa eu vou utilizar os preparadores de stream para uso com o protocolo em tempo real deste projeto.
- [ ] Implementation of the protocol component in Javascript.
- [ ] Protocol tests.

- [ ] Javascript to manage the arrival buffer of video parts on the client side.
- [ ] Testing the video stream with browser. Test object synchronization with video stream.
- [ ] LINUX platform.
- [ ] Optimization and simplification.
- [ ] SSL support for HTTPs.
