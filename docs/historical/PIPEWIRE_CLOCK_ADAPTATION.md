# Adaptation des horloges : point de reprise vérifié

Le backend actuel convertit le format du graphe vers 48 kHz via les adaptateurs
PipeWire, mais ne pilote pas leur taux pour compenser la dérive entre les deux
machines. Une file de taille fixe ne résout pas cette dérive.

La fonction publique `pw_stream_set_rate` est disponible depuis PipeWire 1.4.0
seulement : [documentation officielle](https://docs.pipewire.org/group__pw__stream.html).
Elle ne doit pas être utilisée sans garde sur le poste de qualification en 1.0.5.

Les en-têtes 1.0.5 réellement utilisés sur Linux contiennent déjà le callback
`pw_stream_events.io_changed`, `SPA_IO_RateMatch` et
`SPA_IO_RATE_MATCH_FLAG_ACTIVE`. Ils ne contiennent pas `pw_stream_set_rate`.
La structure RateMatch permet au nœud de demander une correction du
rééchantillonnage existant : [contrat SPA](https://docs.pipewire.org/structspa__io__rate__match.html).
Cette voie évite d'introduire un autre rééchantillonneur dans PWAR.

Avant implémentation, vérifier sur le stream effectivement connecté que
`io_changed` fournit RateMatch, puis le signe de la correction dans chaque sens.
Le sens Windows vers Linux peut être régulé par le remplissage de la file de
lecture Linux. Le sens Linux vers Windows nécessite aussi une information sur
la consommation distante ; la seule file d'émission Linux ne démontre pas
l'équilibre avec l'horloge ASIO Windows. Ne pas annoncer une compensation
bidirectionnelle à partir du seul premier sens.

Ce document identifie le mécanisme à réutiliser, pas une fonctionnalité livrée.
Le binaire remis à Timox et son contrat de contrôle restent inchangés.
