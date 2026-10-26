# Contrôle local PWAR client v1 — implémenté

Le parent lance `PwarAsioClient.exe` sans shell, avec stdin/stdout/stderr
redirigés et `--stdio` à la place de `--seconds`. Il lit stdout et stderr en
continu. Options actuelles : `--driver-clsid`, `--peer`, `--max-queue-ms`
(100 par défaut). Ports local/pair : 8321 ; canaux : 1/2 ; format : float32,
48 kHz ; bloc : celui annoncé par ASIO. Ports et canaux ne sont pas encore
configurables. Reconfigurer signifie arrêter puis relancer, sans changer les
réglages matériels Timox. Aucun endpoint HTTP supplémentaire n'est ouvert.

La bibliothèque JSON est celle déjà présente dans le dépôt, sélectionnée par
`PWAR_JSON_INCLUDE_DIR`. Aucun téléchargement à la compilation.

## Requêtes et réponses

JSON UTF-8, un objet par ligne, au plus 16 384 octets avant le LF. Les requêtes
ont exactement trois champs ; `id` est une chaîne non vide de 128 octets au
plus. Les clés dupliquées et les imbrications excessives sont refusées.

```json
{"version":1,"id":"read-1","command":"status"}
{"version":1,"id":"stop-1","command":"stop"}
```

Réponse réussie : `version:1`, même `id`, `ok:true`, `status` complet.
La réponse à `stop` porte `status.state:stopping`. Erreur :

```json
{"version":1,"id":"bad-version","ok":false,"error":{"code":"UNSUPPORTED_VERSION"}}
```

Autres codes : `INVALID_REQUEST`, `MESSAGE_TOO_LARGE`, `UNKNOWN_COMMAND`.
`id:null` si la requête ne peut pas être identifiée sans ambiguïté. Après une
ligne trop longue, les données sont ignorées jusqu'au LF puis la lecture reprend.

Événements spontanés : `asio_start_accepted`, `state_changed`, `stopped`, avec
`version:1`, `event` et `status`. Un démarrage refusé émet `event:error` avec
`error.code:START_FAILED` et un diagnostic stderr. Le parent accepte les
événements intercalés entre réponses et corrèle celles-ci par `id`.

## Statut complet

- Identité/configuration : `pid`, `driver_clsid`, `sample_rate`, `channels`,
  `block_frames`, `peer_address`, `peer_port`, `local_port`, `max_queue_ms`.
- `state` : `waiting_clock` avant callback ou si le dernier date de plus d'une
  seconde ; `running` avec callback récent ; `error` si erreur réseau fatale ou
  reset ASIO demandé ; `stopped` après fermeture. `stopping` est l'accusé d'arrêt.
- `network_active` : nouveau bloc audio décodé dans la dernière seconde,
  pas une connexion authentifiée. `audio_active` : horloge active et échantillon
  d'amplitude supérieure à 1e-6 dans l'un des sens durant la dernière seconde.
- Compteurs cumulatifs : `callbacks`, `tx_packets`, `rx_blocks`,
  `tx_dropped_frames`, `rx_dropped_frames`, `tx_expired_frames`,
  `rx_expired_frames`, `silenced_frames`, `invalid_packets`, `udp_reset_events`.
  Un bloc peut comprendre plusieurs datagrammes. Les rejets/expirations sont
  locaux ; ils ne mesurent pas toutes les pertes réseau. `udp_reset_events`
  compte WSAECONNRESET, séparément des datagrammes mal formés.
- Détail émission : `tx_queue_overflow_frames`, `tx_send_failures` (nombre de
  datagrammes), `tx_would_block_events` (sous-ensemble WSAEWOULDBLOCK),
  `last_send_error` (dernier code Winsock, zéro si aucun code disponible).
  `tx_dropped_frames` reste l'agrégat des pertes locales d'émission ; ne pas
  additionner cet agrégat à ses composantes.
- `reset_requested`, `network_error` : causes nécessitant un arrêt.
  `last_callback_age_ms:null` avant premier callback.
- `latency_ms:null`, `drift_ppm:null` : ces mesures ne sont pas implémentées.

Les compteurs sont lus atomiquement hors audio sans verrouillage des workers.
Ils ne constituent pas une transaction simultanée entre threads : ne pas
exiger une égalité comptable instantanée entre directions.

Le script `observe-roundtrip.cjs` interroge périodiquement `status` pendant la
fenêtre du signal. Il enregistre chaque réponse dans `pwar-status.jsonl` avec
son `id` de requête, l'horodatage UTC et le temps écoulé depuis le démarrage du
signal. Ces instantanés corrèlent les compteurs cumulés du client aux résultats
des sondes ; leurs compteurs ne sont pas remis à zéro entre deux requêtes.

## Arrêt et vérification

Après `stop` : accusé `stopping`, fermeture ASIO, jonction du worker, événement
`stopped`, sortie. EOF sur stdin effectue le même nettoyage sans réponse de
commande. Un arrêt demandé peut sortir avec succès sans callback : le code zéro
valide alors le cycle de vie, pas l'audio. Le parent doit toujours drainer stdout.

`test-supervision.ps1` teste les vrais pipes : version refusée, status, stop,
événements intercalés et EOF. Il ne modifie ni routes ni configuration moteur.
Les tests C++ couvrent séparément protocole, client UDP et files audio.
