# PWAR : correctifs séparés et qualification

## Réconciliation du 25 octobre 2026

Le correctif additionnel `0014-linux-account-synthesized-silence.patch`
s'applique après le backend expérimental et les files 0011 : il distingue les
frames copiées des zéros produits par la chronologie de la file. Les valeurs
historiques de `played_frames` avant ce correctif ne sont pas directement
comparables aux nouvelles valeurs. Ce patch ne change pas les samples générés
et ne résout pas à lui seul les débordements observés.

Le périmètre autorisé est PWAR uniquement. Le contrat d'intégration cible est
dans [TIMOX_INTERFACING.md](TIMOX_INTERFACING.md) : PWAR doit devenir un client
ASIO de Timox, sans remplacer la carte ASIO physique du moteur. Une première
implémentation en qualification est dans [client/README.md](client/README.md) ;
les tests du pilote PWAR ne remplacent pas ceux de ce client.

Les variantes ont été sauvegardées avec manifestes SHA-256 dans
`build_pwar_isolated_20261025_134451` avant réconciliation. La source réunifiée
est `build_pwar_reconciled_20261025` : patches 0001 à 0013, puis
`windows-receive-burst-capacity.patch` et
`experimental-pipewire-stream-backend.patch`. Les sources des mesures audio
antérieures sont conservées séparément ; leurs résultats ne doivent pas être
attribués automatiquement à la variante réunifiée.

Le patch 0013 a été corrigé après deux échecs reproductibles : la fonction
`pwar_router_process_streaming_packet` adapte les séquences de flux sans
marqueur interne ; le récepteur Linux utilise `pwar_router_process_packet`
pour les réponses fragmentées. Le marqueur `UINT32_MAX` de la première version
est supprimé. SHA-256 du patch révisé :
`2350BE779C64326FF4F25B6F04485AC2F8C7357B8D3066A7A882D9A75A3F7360`.

Vérification : **39/39 tests Windows**, **26/26 tests Linux** avec ASan/UBSan
sur le banc Linux. Le callback avec files est testé en C11 sous Linux avec
son implémentation réelle de file ; l'ancien test C++ reste applicable à
l'ancien callback. Aucun test de callback Linux n'est compté comme exécuté
sous Windows pour cette variante. Les avertissements historiques de compilation
ne sont pas tous résolus. La continuité, la dérive et l'intégration finale
restent non qualifiées. Le bundle distribué à sept patches n'est pas mis à jour
sur la seule foi de ces tests.

## Consolidation du protocole en cours

Le patch candidat `0008-router-preserve-fragment-integrity.patch` s'applique
après les correctifs précédents. Il corrige les fragments de longueur finale
réduite, les doublons, les séquences anciennes, les métadonnées incohérentes,
le démarrage streaming à zéro et les octets non initialisés du routeur émetteur.
La structure UDP reste inchangée. Réinitialiser explicitement le routeur après
redémarrage d'un pair ; la détection réseau de ce redémarrage reste à réaliser.

Les tests sont dans `protocol-audit` et inclus par le CMake de qualification.
La passe consolidée rapporte **34/34** tests locaux. Les preuves, empreintes,
limites et prochaines étapes sont dans
[PWAR_PROTOCOL_REQUALIFICATION.md](../../docs/PWAR_PROTOCOL_REQUALIFICATION.md).
Le bundle actuel à sept patches n'inclut pas encore ce candidat. Aucun résultat
de latence ou de stabilité PipeWire réelle n'est revendiqué.

Base amont : `ripxorip/PWAR`, commit
`ed5b07c44c7635d006422d646d56368600504b3f`.
Les patches modifient PWAR, pas TimoxVasio.dll. Ils conservent les structures
de paquets et les échantillons des échanges valides testés. Une DLL séparée est
compilée pour qualification locale ; elle n'est ni installée ni intégrée dans
Timox, et la liaison avec PipeWire sur le réseau n'est pas encore qualifiée.

Le code amont porte le copyright 2025 Philip K. Gisslow et sa licence est fournie
dans `GPL-3.0.txt`. Les modifications du routeur sont proposées sous cette même
licence. Aucun code PWAR n'est lié au moteur Timox par ce banc de tests.

## 0011 : primitive de file SPSC (candidat, non qualifié)

`0011-linux-frame-queue.patch` ajoute au protocole une file stéréo préallouée
de 4096 frames, avec lecture/écriture SPSC sans attente, conservation des
anciennes frames en cas de saturation et signalement explicite des frames
perdues pour maintenir leur position temporelle par silence. Le banc
`PwarAudioQueueTests` compile cette source et vérifie quanta variables, bornes,
retour arrière, wrap et échange concurrent ; il n'a pas encore été exécuté ici.

Ce patch ajoute la primitive ; son intégration au callback PipeWire et au
worker UDP est proposée séparément en 0012. Aucun des deux n'est inclus dans le
bundle qualifié ni dans `qualification-inputs.json`.

Patch 0012 connects this primitive to the Linux PipeWire callback and UDP
worker. It remains a candidate and is not included in the qualified bundle or
`qualification-inputs.json`; the runtime path has not been built or qualified.

## 0012 : intégration candidate du worker réseau

`0012-linux-queue-network-worker.patch` moves UDP send/receive out of the
PipeWire callback and uses the bounded SPSC queues on both directions. Its
receive path passes fragments unchanged to the protocol router, which owns
sequence and `packet_index` validation for complete blocks. The previous
per-packet sequence check incorrectly rejected later fragments of one block.
The worker uses `recvmsg` with truncation detection, validates fragment counts
before multiplying frame sizes, and handles recoverable socket errors without
spinning on a fatal poll result. The running-state query also follows the
worker's atomic state, so a fatal worker exit is visible to the controller.

Patch 0012 emits 64-frame Linux-to-Windows chunks so the Windows listener can
assemble every supported ASIO block size (multiples of 64). The existing
streaming router API was ambiguous: it inferred fragment indices from sequence
numbers even when a PWAR packet already carried an explicit `packet_index`.
Patch 0013 reserves `UINT32_MAX` as an in-process marker for the ASIO stream
adapter; explicit grouped PWAR fragments keep their protocol indices. The
marker is never sent over UDP, so the wire format stays unchanged.

The Windows listener also checks that a completed router block contains exactly
`blockFrames`. A positive but short or oversized result is replaced with a
silent full block and counted as an xrun, so stale or partial router samples are
never copied as a complete ASIO input block.

When a newer response block arrives, patch 0012 accounts for an incomplete
previous block and any missing 64-frame sequence units by advancing the queue's
timeline with silence and incrementing the xrun counter. A single discontinuity
adds at most 4096 silent frames; late fragments from the old block are rejected
by the router. The Windows listener can still miss a host callback when an
incoming Linux block's final chunk is lost; the next full sequence boundary is
needed before the router can discard that incomplete block and resume.
These candidates are not built, tested, or qualified, and make no claim of
end-to-end audio operation.

## 0001 : contrôles de bornes du routeur

Avant indexation/copie, le receveur vérifie pointeurs, canaux, tailles et nombre
de paquets contre les capacités réelles des tableaux. Le wrapper streaming
vérifie ses pointeurs avant de les lire et utilise une différence de séquences
64 bits ; un paquet refusé reste inchangé. L'émetteur refuse notamment un chunk
nul, un nombre de canaux supérieur à deux et un bloc supérieur à 4096 frames.
Les sorties en erreur gardent un nombre de paquets à envoyer égal à zéro.

Ce patch ne résout pas la division par zéro dans le listener ASIO, qui précède
l'appel au routeur. Il ne résout pas non plus son arrêt concurrent, ses sorties
partielles ni son unique entrée ASIO. Le contrôle de cohérence de tous les
fragments d'une même séquence et les données inutilisées des paquets restent à
examiner. Les pointeurs et capacités de buffers fournis par l'appelant doivent
rester valides pendant tout l'appel.

## Reproduire

Télécharger les sources au commit indiqué dans un dossier de travail distinct.
Conserver leur licence. Avant modification, SHA-256 de `protocol/pwar_router.c`
(octets de l'archive amont, fins de lignes LF) :
`88DA060DE19FF6926A19D7A0545EAD271C8FCAB67FE5FE7DE64F8975DFD376C1`.

Depuis la racine PWAR, appliquer le patch avec son chemin absolu :

```powershell
git apply --check C:/chemin/TimoxVasio/patches/pwar/0001-router-validate-buffer-bounds.patch
git apply C:/chemin/TimoxVasio/patches/pwar/0001-router-validate-buffer-bounds.patch
```

Depuis la racine TimoxVasio, après vérification de la racine et de git status :

```powershell
cmake -S patches/pwar -B build_pwar_qualification -DPWAR_SOURCE_DIR=C:/chemin/PWAR
cmake --build build_pwar_qualification --config Release
ctest --test-dir build_pwar_qualification -C Release --output-on-failure
```

Ce projet CMake séparé compile le véritable `pwar_router.c` de la copie fournie.
Il ne télécharge, n'installe et ne charge aucun pilote. Il évite les cibles
Windows amont, dont les chemins relatifs et la copie post-build vers `Z:` ne
conviennent pas à ce dépôt. Ce n'est pas une correction du build de la DLL.

## Résultats du 24 octobre 2026

- Avant patch : compilation réussie ; test rouge sur l'acceptation d'un paquet
  de zéro frame. Aucun crash provoqué pour cette reproduction.
- Après patch, appliqué avec `git apply --check` puis `git apply` sur une nouvelle
  extraction : compilation MSVC x64 réussie sans avertissement ; CTest 1/1 passe.
- Cas vérifiés : tailles nulles/excessives, indices hors bornes, grandes valeurs
  32 bits, capacité d'émission insuffisante, refus sans modification du routeur
  et du buffer de réception, aller-retour stéréo avec ordre de réception inversé,
  wrapper streaming et blocs maximaux 4096 frames à chunks 64 et 128.

Cette preuve porte sur les fonctions du routeur dans le banc local. Elle ne
qualifie ni l'ensemble des tests amont, ni le transport UDP, le cycle de vie ASIO,
le fonctionnement Linux/Windows ou la latence. L'objectif d'intégration complète
reste décrit dans [l'étude PWAR](../../docs/pwar-integration-assessment.md).

## 0002 : arrêt du listener avant libération

Le constructeur ne lance plus la réception. `start` la lance après préparation
des buffers ; `stop` demande l'annulation puis joint le worker avant de modifier
l'état partagé. `disposeBuffers` attend ce drainage, et le destructeur ferme
l'émetteur après celui-ci. Le drapeau de réception est atomique ; la réception
vérifie l'annulation après un délai socket de 20 ms, y compris sans trafic.
Les datagrammes trop grands et les erreurs UDP récupérables ne ferment pas le
listener. Les mutations de buffers/fréquence pendant la lecture sont refusées.

Le contrat conserve des appels de contrôle sérialisés par l'hôte. `stop` et
`disposeBuffers` refusent l'appel depuis le callback pour éviter un self-join.
Un callback hôte qui ne termine jamais ou un envoi réseau bloqué peut toujours
retenir le drainage : le délai de réception n'est pas une borne universelle
d'arrêt. Le retour d'erreur de bind/startup depuis le worker reste à ajouter.

Appliquer `0002-asio-drain-listener-before-disposal.patch` après 0001 sur la
révision amont, avec les mêmes commandes `git apply --check` puis `git apply`.
Le banc peut construire la DLL et tester directement ses sources Windows :

```powershell
cmake -S patches/pwar -B build_pwar_qualification -DPWAR_SOURCE_DIR=C:/chemin/PWAR -DPWAR_BUILD_WINDOWS_DRIVER=ON -DPWAR_ASIO_SDK_DIR=C:/chemin/asiosdk
cmake --build build_pwar_qualification --config Release
ctest --test-dir build_pwar_qualification -C Release --output-on-failure
```

Le SDK ASIO existant est utilisé par chemin, sans copie ni téléchargement.
La cible ne lance pas les commandes d'installation ou d'enregistrement du pilote.
Le test Windows lie les vraies sources du pilote dans un exécutable : il ne
prouve pas l'activation COM de la DLL construite. Exécuter via CTest, qui configure
un profil de test local avec destination audio 127.0.0.1 ; les logs sont aussi
dirigés vers loopback. Il vérifie que le port UDP 8321 est libre avant de démarrer,
puis l'ouvre temporairement. Aucun flux matériel n'est activé.

Validation observée le 24 octobre :

- Amont : test bloqué à la destruction, timeout CTest à 5 secondes.
- Première correction : la revue a détecté un arrêt sur WSAEMSGSIZE ; test rouge
  reproduit, puis correction et test vert.
- Deux patches appliqués sur une nouvelle extraction : build x64 réussi, puis
  CTest **2/2**, 0,26 seconde. Avertissements amont/SDK toujours présents
  (conversions, noms masqués et anciens exports du fichier .def).
- Contrôles : arrêt sans trafic, port ouvert/libéré, redémarrage, datagramme
  surdimensionné ignoré, libération retenue pendant un callback actif, destruction
  directe d'un listener actif. Revue indépendante sans autre défaut important
  dans ce périmètre.
- `build_pwar_qualification/Release/PWARASIO.dll`, 148992 octets, timestamp UTC
  `2026-10-24T09:36:11`, SHA-256
  `6F792BAF5B3380895CB8F3E65C8CD97878AA1AF3F2BA19046266E68C13ECF443`.
  Exports COM examinés avec dumpbin, sans chargement ni enregistrement de la DLL.

Après 0002 restent notamment la validation du listener (traitée par 0003 ci-dessous),
les sorties partielles, la capture stéréo, le résultat de démarrage, la cohérence des fragments,
les horodatages partagés et les essais avec Linux. Ce binaire de qualification
ne constitue pas une livraison utilisable du pont.

## 0003 : validation du datagramme avant calcul

Appliquer `0003-asio-validate-datagrams-before-division.patch` après 0002, avec
`git apply --check` puis `git apply`. Le listener exige exactement la taille
du paquet structuré existant. Il refuse un chunk inférieur à 64, supérieur à
128, supérieur au bloc ASIO local, ou ne divisant pas exactement ce bloc.
Ces contrôles précèdent toute division, mise à jour de latence ou mutation
du routeur. Aucun champ ni format réseau n'est ajouté.

Le test du vrai listener a reproduit une exception numérique avant correction.
Après correction, il envoie les tailles 0, 1, 63, 96, 129 et 65535, puis des
datagrammes tronqués et prolongés, avec une observation sans callback de 40 ms
après chaque envoi. Le port doit rester ouvert. Le paquet valide 128 frames de
l'essai de drainage déclenche ensuite le callback. Les envois sont espacés pour
réduire les pertes dans la petite file UDP ; ceci ne prouve pas la livraison de
chaque paquet et ne remplace pas une campagne de fuzzing.

Application du patch vérifiée sur la copie contenant 0001+0002 ; contenu obtenu
identique au source compilé après normalisation des fins de lignes. Revue sans
défaut nouveau trouvé. La DLL x64 à trois patches mesure 148992 octets, timestamp
UTC `2026-10-24T09:47:02`, SHA-256
`2D3EFDEDBF01B716F20A61C0651310762A83511AC54DCBCFBE2E301FF2A01345`.
L'empreinte de la section 0002 décrit uniquement l'artefact antérieur.

Après 0003, les canaux partiels et la capture stéréo restent à traiter (voir
0004 ci-dessous), ainsi que le retour d'erreur au démarrage et les essais Linux.
Les tests présents n'établissent pas une
compatibilité audio réseau complète ni une performance temps réel.

## 0004 : stéréo et sélections partielles de canaux

Appliquer `0004-asio-map-stereo-and-partial-channels.patch` après 0003. Le pilote
expose deux entrées ASIO correspondant aux deux canaux déjà présents sur le
réseau. Les copies utilisent les numéros de canaux `inMap`/`outMap` au lieu de
supposer que les slots ASIO sont dans l'ordre réseau. Les sorties non ouvertes
sont mises à zéro : une seule sortie ou une configuration capture seule ne lit
plus un pointeur de sortie absent. Les buffers ASIO sont initialisés à zéro.
La sélection entière est validée avant allocation/indexation : canaux hors
plage et doublons sont refusés.

`udp_send_port` devient une option du fichier `pwarASIO.cfg`, entre 1 et 65535.
Sans option valide, la destination reste 8321 ; le port de réception reste 8321.
Le banc CTest configure `udp_send_ip=127.0.0.1` et `udp_send_port=18321` pour
capturer les véritables datagrammes émis sur un socket distinct. Cette option
change l'adresse de transport, pas le format PWAR. Aucun fichier de configuration
utilisateur existant n'est modifié.

Les huit cas réseau testés sont stéréo, entrées/sorties inversées, sorties seules
inversées avec une entrée, gauche seule, droite seule, capture seule, lecture
seule et entrée privée d'horloge. Ce dernier cas ouvre l'entrée 0, comme l'hôte
Timox sans route ; ce n'est pas une ouverture à zéro descripteur, toujours refusée.
Chaque cas envoie deux blocs de 128 frames avec amplitudes distinctes pour
vérifier les deux moitiés ASIO. Le callback contrôle les échantillons d'entrée ;
le socket du banc contrôle les échantillons et les canaux des paquets de sortie.
Le banc distingue les messages de statistiques existants par leur taille.

Avant correction : ouverture stéréo refusée, échantillons de sorties inversées
incorrects ; essai gauche seule terminé par le timeout CTest de 5 secondes.
Après correction : compilation de la DLL et **CTest 10/10**, 1,61 seconde.
La revue a identifié la nécessité de distinguer les statistiques dans le test
sur deux blocs ; le banc a été corrigé puis toute la suite relancée.
Le patch appliqué à la copie 0001+0002+0003 produit les sources compilées
identiques après normalisation des fins de lignes.

Artefact local avec quatre patches :
`build_pwar_qualification/Release/PWARASIO.dll`, 148992 octets,
timestamp UTC `2026-10-24T09:57:50`, SHA-256
`F43AAEBA17C43DF2E081837CE4C3A3D7939CD01118AC3567B2F6C642A4E42A95`.
Les empreintes précédentes sont historiques. Aucun enregistrement COM, aucune
installation ni qualification avec un hôte Linux n'a eu lieu.

### Limites du contrôle de paquets

Le listener contrôle taille et nombre d'échantillons avant les calculs. Le
routeur contrôle les bornes des tableaux et des indices. La distinction des
structures audio/statistiques repose sur leur taille dans le protocole amont.
Ces contrôles n'authentifient ni l'émetteur ni la nature déclarée des données :
un datagramme de bonne taille aux champs plausibles peut les franchir. Aucun
magic, version ou mécanisme d'authentification nouveau n'est ajouté au protocole.
La cohérence des fragments d'une même séquence et les horodatages partagés
restent à corriger/qualifier, ainsi que les
essais Windows/Linux et les mesures de dérive, perte et latence sur durée.

## 0005 : signaler l'échec d'ouverture du listener

Appliquer `0005-report-listener-startup-failure.patch` après 0004. `start()`
attend sur le thread de contrôle le résultat de l'initialisation et du bind UDP.
Les échecs explicites du setup sont publiés par une promise ; le worker est
joint avant le retour `ASE_HWMalfunction`. `getErrorMessage()` indique le
listener UDP et le port 8321. Une nouvelle tentative sur la même instance
fonctionne après libération du port. Aucun changement du protocole réseau.

Le test réserve réellement 8321 avec `SO_EXCLUSIVEADDRUSE` : avant correction,
échec sur l'assertion du retour ASIO ; après correction, **CTest 10/10**, 1,66 s.
La copie 0001–0004 accepte le patch et produit les mêmes sources que le build,
après normalisation des fins de lignes. Revue sans défaut important sous
l'hypothèse des appels de contrôle ASIO sérialisés par l'hôte.

Artefact local avec cinq patches : `build_pwar_qualification/Release/PWARASIO.dll`,
169472 octets, timestamp UTC `2026-10-24T11:34:18`, SHA-256
`B07C7BBFAEF9FA1A5A8945EB15C54537E7475CC7EF9C0C6E216C5E197B619F6C`.

L'hôte Timox vérifie déjà le retour de `driver->start()` et le contrôleur passe
en erreur ; à cette étape son message est générique (`ASIO start failed`, code ASIO).
Cette propagation a été vérifiée par lecture, pas par chargement COM de cette
DLL. Le diagnostic détaillé PWAR n'est pas encore repris par l'hôte.
Le test ne prouve pas les autres pannes WinSock ni une borne temporelle de
démarrage (ordonnancement, journalisation amont). Une défaillance après bind,
le socket d'émission et la réception réelle depuis Linux restent à qualifier.
Aucune installation ou inscription COM n'a été effectuée.

## 0006 : journalisation réseau désactivée et essai de la DLL

Appliquer `0006-disable-network-logging-by-default.patch` après 0005.
Les méthodes de journalisation deviennent inertes par défaut : aucun socket,
verrou, allocation ou envoi depuis ces méthodes. L'amont utilisait une adresse
fixe `10.0.0.171:1338`, également depuis le traitement audio. Le comportement
historique reste disponible en définissant explicitement `PWAR_ENABLE_UDP_LOGGING`
pour un build de diagnostic ; il n'est pas qualifié pour le temps réel.
La télémétrie de latence du protocole PWAR est inchangée.

Le nouveau banc `PwarAsioDllTests` ne lie aucun code d'implémentation PWAR :
il charge le chemin absolu `$<TARGET_FILE:PwarQualificationDriver>`, obtient
IASIO via `DllGetClassObject` et sa fabrique, puis utilise les méthodes ASIO.
Les huit modes de canaux partagent les assertions du banc source : deux blocs
UDP aller-retour, échantillons d'entrée et de sortie, ordre des canaux et silence
sur les sorties absentes. Il arrête, libère et décharge la DLL en fin de portée.
Le profil utilisateur reste temporaire ; aucune inscription COM n'est effectuée.

Compilation finale réussie avec `/EHsc` pour le banc DLL, **CTest 18/18**.
Revue sans défaut important ; application du patch à la copie 0001–0005 comparée
au fichier compilé après normalisation des fins de lignes.

Artefact local avec six patches : `build_pwar_qualification/Release/PWARASIO.dll`,
166912 octets, timestamp UTC `2026-10-24T12:05:29`, SHA-256
`EAB1067A785D3AA25A241D0DD3122263558D555459879060866AA62B0D982EB2`.
Le test prouve le chargement via la fabrique exportée et le chemin audio local,
pas la découverte par le registre Windows, l'utilisation par l'hôte Timox ou
une liaison avec PipeWire sous Linux. Ces étapes et les performances restent
à qualifier avant distribution.

### Diagnostic transmis par l'hôte Timox

L'hôte conserve maintenant le texte de `getErrorMessage()` après un échec de
`createBuffers()` ou `start()`, avant toute libération susceptible de l'effacer.
L'opération et le code ASIO restent présents même si le pilote ne fournit aucun
texte. Un octet sentinelle borne la lecture des 124 octets du contrat ASIO.
Cette fonction s'exécute sur le thread de contrôle, jamais dans le callback audio.

`AsioErrorTests` vérifie message vide, détail fourni et tampon rempli sans zéro
terminal. Les huit tests de DLL réservent aussi le port 8321, lisent le diagnostic
via la même fonction que l'hôte, libèrent le port puis testent l'audio sur la même
instance. **8/8 réussis**, moteur Release compilé. Le contrôleur transmet déjà son
erreur au contrat API ; le parcours complet avec pilote enregistré et GUI reste
à tester. Il ne s'agit pas encore d'un test du routage Timox avec PWAR.

### Hôte ASIO Timox avec DLL PWAR

`PwarTimoxHostTests` compile les sources de production `physical_asio_host.cpp`
et `asio_sample_conversion.cpp`, sans modification. Seul l'adaptateur
`AsioDriverList` est remplacé dans le banc : il présente un inventaire isolé
et active la DLL par sa fabrique exportée. Aucune clé de registre n'est écrite.

Trois cas passent : stéréo, canal droit seul, horloge sans route (buffer privé).
Chaque cas vérifie l'ouverture, la fréquence 48000 Hz, l'échec avec réservation
exclusive de 8321 sur toutes les interfaces, le diagnostic détaillé et une
nouvelle tentative sur le même hôte. Deux blocs de 128 frames passent par les
conversions et le callback de l'hôte ; le banc contrôle chaque échantillon en
entrée puis en sortie (gain x2), ainsi que le silence des sorties absentes.
Les compteurs confirment deux callbacks ; arrêt et fermeture libèrent le port.

Compilation et **CTest 21/21**, 2,17 s. Les premiers essais du banc ont été
corrigés pour réserver l'adresse wildcard (une réservation loopback seule ne
garantit pas le conflit) et lire les compteurs avant l'arrêt, conformément au
contrat existant. Aucun changement du moteur ou de la DLL n'a été nécessaire.

Cette preuve couvre l'hôte ASIO réel, pas encore le graphe de routage complet,
le contrôleur/API/GUI, la découverte par le registre ou un pair Linux.
Le hash de la DLL à six patches ci-dessus reste celui testé. La stabilité sur
durée et les performances au niveau de TimoxVasio ne sont pas établies.
