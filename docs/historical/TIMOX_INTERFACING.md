# PWAR : contrat d'intégration avec TimoxVasio

Statut : contrat cible, pas une déclaration de fonctionnalité livrée.
Périmètre de cette intervention : PWAR, ses tests et cette documentation.
Les autres composants TimoxVasio sont consultés uniquement en lecture.

Une première implémentation autonome existe dans `client/asio_client.cpp`.
Elle charge le pilote enregistré par son CLSID, utilise ses canaux float32
stéréo et son bloc préféré, et refuse explicitement une fréquence autre que
48 kHz. Elle ne dépend d'aucune source du moteur Timox. Elle est encore en
qualification : compensation de dérive, reprise automatique et contrôle
complet ne sont pas implémentés.

Essai d'interfaçage du 25 octobre : le pilote installé 1.2.7 accepte
l'ouverture et le démarrage, et l'API Timox découvre `PwarAsioClient.exe` avec
les entrées et sorties 1/2. Deux essais de 10 puis 30 secondes ne reçoivent
aucun callback. Dans la même observation, `engine.state` vaut `running`,
le pilote physique sélectionné reste SSL ASIO Driver 1, mais `bufferHealth`
rapporte zéro callback sur une fenêtre de 5 012 ms. La fréquence reste
48 kHz, le bloc 2 048 frames et la révision de configuration 2.
Cela établit la découverte du client, pas un fonctionnement audio.

**Attente d'interfaçage à résoudre côté Timox :** un client ASIO démarré doit
recevoir les callbacks de l'horloge moteur, même sans signal, ou obtenir un
état explicite d'horloge indisponible. Vérifier que le compteur moteur et celui
du client progressent ensemble tout en conservant la carte locale. La cause
interne de l'absence d'horloge n'est pas établie par cet essai ; aucun correctif
du moteur n'est effectué dans cette intervention PWAR.

## Point d'entrée existant

Le pont Windows PWAR doit être un **client du pilote ASIO TimoxVasio** dans
son propre processus. Il apparaît comme une application routable ordinaire.
Il ne sélectionne pas PWAR ASIO comme carte physique du moteur Timox et ne
ferme pas la carte ASIO matérielle choisie par l'utilisateur.

Le pilote ASIO PWAR historique reste un produit distinct : les essais où
PhysicalAsioHost charge PWARASIO.dll ne prouvent pas cette intégration finale.
La mémoire partagée AudioClientMapping est explicitement privée dans
`include/audio_transport.h` : le pont ne doit pas en dépendre directement.

| Flux | Interface du client ASIO PWAR | Destination |
|---|---|---|
| Application Windows vers Linux | Entrées ASIO du client, alimentées par le graphe Timox | File émission puis réseau, sortie PipeWire |
| Traitement Linux vers Windows | Réseau puis file réception | Sorties ASIO du client, sources routables dans Timox |

Les deux directions fonctionnent simultanément, y compris si l'une transporte
du silence. Pas de réponse obligatoire à un paquet reçu pour produire l'autre
direction. Le client démarre sur l'horloge ASIO Timox, pas sur l'arrivée UDP.
Les callbacks copient des frames dans des files préallouées ; aucun réseau,
verrou, journal, allocation ou attente de l'autre machine dans ces callbacks.

## Formats, horloges et cycle de vie

- Négocier les canaux, le format d'échantillon, la fréquence et le bloc auprès
  du pilote. Convertir côté PWAR ; ne pas changer la configuration physique
  Timox pour satisfaire le réseau. Stéréo initiale explicitement annoncée.
- Les adaptateurs PipeWire traitent le format du graphe Linux ; cela ne suffit
  pas à synchroniser deux horloges physiques indépendantes. PWAR doit assurer
  une adaptation de dérive mesurée et une latence bornée.
- Rebloquer les frames indépendamment des datagrammes de 128 frames. Toute
  configuration non prise en charge échoue explicitement avant activation.
- À la perte du réseau, rendre du silence et poursuivre la cadence Timox.
  À la reprise, écarter les données périmées avant de réamorcer. Ne pas rejouer
  un retard accumulé. Compter pertes, silences, rejets et réamorçages séparément.
- Traiter arrêt, reset ASIO, changement de fréquence et fermeture hors callback.
  La fermeture attend la fin des callbacks avant de libérer leurs buffers.

## Contrôle et télémétrie

Le pont PWAR est propriétaire de ses paramètres réseau et de ses compteurs.
Son interface de contrôle doit distinguer configuration demandée et effective,
état local, activité du pair et activité audio ; un socket ouvert ne prouve pas
une connexion ni un flux. Fournir un instantané cohérent, versionné, hors RT :
fréquence/canaux/bloc effectifs, paquets et frames transmis/reçus, pertes,
doublons, rejets tardifs, sous/sur-remplissages, silences, remplissage des files,
réamorçages, dernière activité, erreur et mesure de latence avec son unité.
Une valeur de latence indisponible reste indisponible, jamais zéro par défaut.
Une mesure aller-retour n'est pas présentée comme une latence aller simple.

Constat au 25 octobre 2026 : `GET/PUT /api/v1/pwar` dans
`src/control_api_server.cpp` lit/écrit seulement adresse et port, avec
`reopenRequired`. Ce contrat ne couvre ni activation indépendante, ni compteurs
réseau. Il ne faut pas lui attribuer des capacités absentes.

Attente à transmettre au responsable Timox si l'exposition dans son API est
requise : consommer l'interface de contrôle PWAR et publier ces états sans
modifier leur signification ; préserver l'authentification du contrôle distant.
Le format concret de cette interface doit accompagner son implémentation PWAR,
avec exemples et tests ; aucun nouvel endpoint Timox n'est implémenté ici.
Le routage audio peut déjà utiliser les endpoints et commandes des clients ASIO
existants. Les modifications de routage doivent préserver les autres clients
et respecter la révision de configuration de l'API.

## Preuves requises

1. Un client PWAR charge le pilote Timox, apparaît dans son graphe et fonctionne
   pendant que la carte ASIO locale continue de tourner.
2. Application Windows → entrée PWAR → PipeWire → traitement connu → sortie
   PWAR → destination Timox : vérifier identité des canaux et signal retourné.
3. Tester les fréquences et quanta annoncés, dérive longue durée, rupture réseau,
   reprise et resets ASIO, avec seuils de latence et de pertes explicités.
4. Confronter chaque compteur à une faute injectée. Tester le contrôle depuis
   Linux authentifié et le refus d'un accès non authentifié via l'API Timox.

Les tests actuels de DLL PWAR et les essais Linux → hôte Windows → Linux sont
des prérequis ; ils ne valident pas encore ces quatre critères.
