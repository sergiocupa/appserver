# AppServer
HTTP, Application and Video Stream server. Can also be used as a WebAPI.
For use in embedded systems, but can also be used as a micro WebAPI.
At the moment, 18/03/2025, the implementation is not complete.

Features to be implemented:

- [X] Finalize parser and tests;
- [X] Method mapping, to forward actions to requests;
- [X] Create method mapping tests;
- [X] Download resources and HTML pages for presentation in browser;
- [X] Browser tests
- [X] Handshake for websocket protocol;
- [ ] Define data header for client-side multiplexing for websocket connection. Implement multiplexed response to client;
Javascript for client-side multiplexing for websocket connection;
- [ ] Websocket tests;
- [ ] Study and subsequent implementation of an H.264 video fragmenter. Fragments to be dispatched using a routed websocket connection for presentation in browser;
- [ ] Javascript to manage the arrival buffer of video parts on the client side;
- [ ] Testing the video stream with browser;
- [ ] LINUX platform;
- [ ] Optimization and simplification;
- [ ] SSL support for HTTPs;
