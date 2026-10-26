# Essai réel du client PWAR — 25 octobre 2026

Statut : transport observé, stabilité non qualifiée. Aucun composant, route
ou paramètre moteur Timox n'a été modifié par cet essai.

## Provenance et montage

- Windows : `build_pwar_reconciled_tests/Release/PwarAsioClient.exe`, SHA-256
  `1622AD892BEF6AAFBC4E4CF8C9F3626633AC111D0FBCE2FB296A412C3223C686`.
- Linux : source réunifiée, `pwar_stream_cli` recompilé, SHA-256
  `c247b354aba58cb10505b3bf96d1af2fd1a38390f36459b22bbdcdaef780818a`.
- Timox : pilote virtuel installé 1.2.7, carte physique SSL ASIO Driver 1,
  48 kHz / 2048 frames. Client ASIO PWAR indépendant, canaux 1/2.
- Linux : source 440 Hz dans PipeWire vers `pwar-send`, réception Windows
  `pwar-receive` reliée à un enregistreur. Le transport réseau est stéréo 48 kHz.
- Le client fonctionne en mode supervisé ; relevés `status` toutes les cinq
  secondes pendant trente secondes, puis fermeture du pipe parent.

Les routes Timox étant inchangées, le flux Windows vers Linux contient du
silence. Cet essai ne constitue pas l'aller-retour traité d'une application
Windows. La source Linux non silencieuse est toutefois reçue par le client ASIO.

## Observations

Les six relevés indiquent `running`, `network_active:true`, `audio_active:true`.
Le bilan final du client indique :

| Compteur | Valeur |
|---|---:|
| Callbacks ASIO | 703 |
| Paquets émis avec succès | 11 118 |
| Blocs reçus | 11 372 |
| Frames rejetées à l'émission | 16 640 |
| Frames rejetées pour débordement de réception | 0 |
| Frames expirées dans la file locale de réception | 12 544 |
| Frames expirées dans la file locale d'émission | 0 |
| Frames de silence ajoutées en sortie ASIO | 0 |
| Datagrammes invalides / resets UDP | 0 / 0 |

703 × 2048 / 128 = 11 248 paquets attendus : l'écart de 130 paquets correspond
exactement aux 16 640 frames rejetées. Ce compteur agrège encore échec d'envoi
et débordement de file : il faut les distinguer avant d'attribuer la cause.
Les expirations en réception atteignent 12 416 au premier relevé, 12 544 au
deuxième, puis restent constantes. Elles ne prouvent pas une latence réseau.

Linux rapporte 11 118 paquets reçus pendant les intervalles ayant une activité
de réception, soit le même nombre que les envois réussis Windows. Sur ces
31 intervalles de rapport d'une seconde (incluant les frontières de l'essai),
il compte 265 600 frames rejetées en réception et 64 916 frames explicitement
silencieuses. Ces fenêtres ne sont pas exactement la durée ASIO.

Attention : `pwar_audio_queue_read` Linux peut retourner des frames de silence
synthétisées pour les trous de sa chronologie. Le backend ajoute actuellement
tout ce retour à `played_frames`. Il ne faut donc pas interpréter cette valeur
comme un nombre de frames audio reçues et effectivement conservées. La mesure
doit distinguer données copiées, zéros synthétisés et manque non couvert.

Après l'essai, l'API moteur indique `healthy`, 117 callbacks sur sa fenêtre
d'environ cinq secondes et zéro dépassement de traitement. Le client s'arrête
avec le code zéro ; le relais et l'enregistreur de qualification sont arrêtés.
Ces observations ne remplacent pas une vérification de continuité de la carte.

## Suites nécessaires

1. Distinguer erreurs socket d'émission et débordements de la file Windows.
2. Corriger la comptabilité des silences et qualifier les files Linux avec les
   rafales de 2048 frames produites par Timox et les quanta PipeWire effectifs.
3. Mesurer le signal de bout en bout, les latences, la dérive et la reprise.
4. Raccorder le contrôle supervisé PWAR à l'API Timox via son agent responsable.

La capture de statuts locale se trouve dans
`build_pwar_reconciled_tests/real-duplex-observation.json`. Les tests de protocole
et de pipes précédents sont des preuves distinctes de cet essai réseau.
