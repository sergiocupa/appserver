# AppServer
HTTP, Application and Video Stream server. Can also be used as a WebAPI.
For use in embedded systems, but can also be used as a micro WebAPI.
At the moment, 13/03/2025, the implementation is not complete.

Features to be implemented:

- [X] Finalize parser and tests;
- [X] Method mapping, to forward actions to requests;
- [X] Create method mapping tests;
- [X] Download resources and HTML pages for presentation in browser;
- [ ] Browser tests
- [ ] Handshake for websocket protocol;
- [ ] Javascript for routing the client side of the websocket connection, for bidirectional communication using mapping;
- [ ] Websocket tests;
- [ ] Study and subsequent implementation of an H.264 video fragmenter. Fragments to be dispatched using a routed websocket connection for presentation in browser;
- [ ] Javascript to manage the arrival buffer of video parts on the client side;
- [ ] Testing the video stream with browser;
- [ ] LINUX platform;
- [ ] Optimization and simplification;
- [ ] SSL support for HTTPs;
