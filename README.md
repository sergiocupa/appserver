# AppServer
General purpose HTTP server. Can also be used as a WebAPI. 
For use in embedded systems, but can also be used as a micro WebAPI.
At the moment, 26/02/2025, the implementation is not complete.

Features to be implemented:

- Finalize parser tests.
- Implement method mapping to forward actions to requests.
- Implement method mapping tests.
- Implement download of resources and HTML pages for presentation in browser.
- Implement websocket.
- Implement Javascript for routing on the client side of the websocket connection, for bidirectional communication using mapping.
- Implement websocket tests.
- Studies and implementation of the H.264 video fragmenter. Fragments to be dispatched using a routed websocket connection for presentation in browser.
- Implementation of JAVASCRIPT to manage the buffer for arrival of parts of the video on the client side.
- Implement tests of the video stream with browser
