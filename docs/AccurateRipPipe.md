%%{init: {'flowchart': {'htmlLabels': true, 'padding': 18}}}%%

graph TD

&#x20;   classDef hardware fill:#13161b,stroke:#4fc8a0,stroke-width:2px,color:#cdd5df;

&#x20;   classDef handshake fill:#1a1e25,stroke:#c98bff,stroke-width:2px,color:#cdd5df;

&#x20;   classDef forensic fill:#111418,stroke:#f0c060,stroke-width:2px,color:#cdd5df;

&#x20;   Start(\[START RIP]) --> A\[Phase 01: Disc ID Gen]

&#x20;   A --> B\[Phase 02: WinInet Handshake]

&#x20;   B --> C{HTTP 200?}

&#x20;   C -- Yes --> D\[Phase 03: .bin Binary Parse]

&#x20;   C -- No/404 --> F(\[NOT FOUND])

&#x20;   D --> O\[Phase 04: Drive Offset Correction]

&#x20;   O --> P\["Phase 04a: Pregap Preamble Read<br/>150 sectors before track start"]

&#x20;   P --> E\["Phase 04b: CRC Accumulate<br/>disc-absolute mul\_by anchoring"]

&#x20;   E --> G\[Phase 05: Verify CRCv1/v2 vs DB]

&#x20;   G --> H{Match Found?}

&#x20;   H -- Yes --> I(\[VERIFIED: BIT-PERFECT])

&#x20;   H -- No --> J{Retry Pass 2?}

&#x20;   J -- Yes --> K\["Flush Hardware Cache<br/>Reduce to 1x Speed"]

&#x20;   K --> P

&#x20;   J -- No --> L(\[ABORT: VERIFY FAIL])

&#x20;   class A,B,D,O hardware;

&#x20;   class C,G,H,J handshake;

&#x20;   class P,E,K forensic;



