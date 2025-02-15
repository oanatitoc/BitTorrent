# BitTorrent

#### Titoc Oana Alexandra 333CA

# Assignment #2 The BitTorrent Protocol

# Main functionalities of the tracker, clients, and download/upload threads

1. Tracker

The tracker manages the state of files and swarms in the network.

Main functions:

-   **tracker_recv_files**: Receives the files originally held by each client and creates the swarms of files that contain the list of seeds and the list of hashes. (Receives MSG_INIT and responds with MSG_INIT_ACK).
-   **tracker_handle_request_swarm**: Responds to a swarm's request for a file, meaning it sends both the list of hashes and the list of seeds. (Receives MSG_REQUEST_SWARM and responds with MSG_SWARM_INFO).
-   **tracker_handle_request_seeds**: Responds to a request with only the list of seeds for a file. (Receives MSG_REQUEST_SEEDS and responds with MSG_SWARM_INFO).
-   **tracker_handle_finish_file**: Updates the swarm when a client finishes downloading a file. (Receives MSG_FINISH_FILE).
-   **tracker_handle_finish_all**: Checks if all clients have completed their downloads. (Receives MSG_FINISH_ALL and sends MSG_TERMINATE).

2. Download Thread

The client's download thread manages the logic of obtaining the desired files. It:

*   Requests the swarm of the desired files from the tracker (MSG_REQUEST_SWARM).
*   Receives the hashes of the segments and the list of clients (swarm).
*   Requests segments from peers/seeds using MSG_REQUEST_SEG.
*   Requests the updated list of seeds from the tracker after every 10 downloads (MSG_REQUEST_SEEDS).
*   Notifies the tracker when a file is completely downloaded (MSG_FINISH_FILE).
*   Notifies the tracker when all desired files have been downloaded (MSG_FINISH_ALL).

In the `wanted_file_t` structure, we keep the name of the desired file, how many segments it has, which ones they are, and whether they have been downloaded (an integer array). When the download function requests the swarm of a file for the first time (meaning when we are asking for both the list of seeds and the list of hashes), it populates specific fields for the hashes in this structure. Thus, we will mark the downloading of a file by populating the `have_chunk` field in this structure with 1 when an OK is received from the seed/peer from which we have downloaded. After every 10 segments downloaded, only the updated list of seeds/peers is requested in a cyclic manner.

3. Upload Thread

The client's upload thread responds to segment requests from other clients. It:

*   Receives segment requests (MSG_REQUEST_SEG).
*   Checks if it owns the requested segment.
*   Sends an acknowledgment (MSG_ACK_SEG) if the segment is available or a negative response (NA) if the segment does not exist.
*   Closes when the tracker sends the terminate signal (MSG_TERMINATE).

4. Client

The client combines the functionality of the download and upload threads. It:

*   Sends the tracker information about the files it holds (MSG_INIT).
*   Initiates and manages threads for download and upload.
*   Closes the application when all files have been downloaded and the termination signal has been received.

Main functions:

-   **init_client**: Initializes the client structure and sends the initial information to the tracker.
-   **peer**: This calls init_client and manages the download and upload threads.
