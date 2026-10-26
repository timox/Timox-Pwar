# Client ASIO PWAR Windows

Le mode supervisé stdin/stdout est documenté dans [CONTROL.md](CONTROL.md).
Il fournit les commandes versionnées `status` et `stop`, ainsi que l'arrêt sur
fermeture du pipe parent. Le raccordement à l'API Timox n'est pas implémenté ici.

Client autonome en qualification, construit par le CMake PWAR avec
`PWAR_BUILD_WINDOWS_DRIVER=ON` et un SDK ASIO local. Il n'installe aucun pilote
et ne modifie ni fréquence, ni bloc, ni routage Timox.

Exemple avec le pilote TimoxVasio enregistré :

```powershell
.\build_pwar_reconciled_tests\Release\PwarAsioClient.exe `
  --driver-clsid '{A4D39126-78CB-4D89-9E0A-54494D4F5856}' `
  --peer 10.1.0.101 --seconds 30
```

Le pair doit exécuter le backend PWAR PipeWire bidirectionnel. UDP 8321 doit
être disponible côté Windows : ne pas ouvrir simultanément le pilote ASIO
PWAR historique, qui utilise le même port. Le client ouvre les canaux ASIO
1/2 dans chaque direction. Les entrées ASIO vont au réseau ; les paquets reçus
alimentent les sorties ASIO. Les routes sont celles du graphe Timox existant.

Les files SPSC préallouées adaptent les tailles de blocs entre le callback et le worker
UDP ; elles sont testées avec deux threads, des blocs 97/128 et 500 000 frames.
`asio_start_accepted` signifie uniquement que le pilote a accepté `start()`.
Le bilan distingue callbacks, paquets transmis, blocs reçus, frames rejetées,
silences et datagrammes invalides. Un essai sans callback sort en échec.

`--max-queue-ms` limite l'âge dans chacune des deux files locales (100 ms par
défaut, plage 1..2000, au moins la durée d'un bloc ASIO). Les frames expirées
sont retirées par leur consommateur et comptées dans `tx_expired_frames` et
`rx_expired_frames`, indépendamment des débordements. La file ne se réinitialise
jamais pendant son utilisation. La précision suit l'horloge monotone locale
Windows ; l'égalité au seuil reste admissible. Ce contrôle ne mesure pas le
retard accumulé avant réception sur le réseau et ne garantit pas la latence
de bout en bout ni la continuité audio.

`PwarAsioClientTests` compile le vrai client, remplace l'activation COM par un
pilote simulé et permet l'injection explicite d'un échec d'envoi. Le test utilise
de vrais sockets UDP locaux et vérifie
le découpage 2048 → 16 × 128, le retour stéréo, le silence après épuisement,
le rejet d'un datagramme mal formé, la notification de reset, le refus de
44,1 kHz et le nettoyage après échec de démarrage. L'arrêt est vérifié par
la libération du port et la terminaison du worker ; il est idempotent.
Ce test passe le 25 octobre 2026. Il ne mesure ni la dérive ni le réseau réel.

Limites actuelles : stéréo float32 little-endian à 48 kHz, bloc préféré maximal
4 096 frames ; pas encore de compensation de dérive, de rejet des paquets déjà anciens à leur arrivée,
de reprise automatique après reset ASIO ou redémarrage du pair. Les files sont
bornées, mais cela ne garantit pas encore une latence acceptable. L'adresse
source est filtrée ; le protocole UDP historique n'est pas authentifié.
Ne pas utiliser cette version comme preuve de stabilité ou comme distribution.
