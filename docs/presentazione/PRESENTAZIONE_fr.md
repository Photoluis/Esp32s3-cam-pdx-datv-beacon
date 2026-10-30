# ESP32-S3-CAM_PDX DATV Beacon

**Un émetteur DVB-S2 complet, avec caméra, sur une carte de quelques dizaines d'euros**

*Luigi D'Arcangelo, IZ7PDX — projet open source, licence GPL-3.0*

![Réception de la balise avec Portsdown 4 et Minitiouner : MER 26 dB](../img/test_mer_20261030/05_m9.6dB_MER26.0.jpg)

Une seule **carte ESP32-S3 avec caméra** devient une balise de télévision numérique d'amateur. L'émetteur Wi-Fi de la
puce sert de **modulateur I/Q** ; tout le reste — vidéo, multiplexeur MPEG-TS, codage **DVB-S2** selon ETSI EN 302 307-1
et modulation QPSK — est calculé par la carte elle-même. Ni SDR, ni PC, ni Raspberry Pi : une alimentation, une antenne
et un filtre.

## Ce qu'elle fait

- **DVB-S2 QPSK** à 333 kS/s, FEC de 1/2 à 9/10 : reçu avec un **MER de 23 à 27 dB** sur Portsdown 4 et Minitiouner.
- **2300–2450 MHz** en direct ; toute autre bande avec un **mélangeur externe** : la fréquence finale s'affiche à l'image
  et dans le nom du service.
- **Caméra en direct** (H.264), caméra en **incrustation** (PiP) sur la mire, mire standard H.265 et **mires
  personnalisées** : une photo depuis le téléphone ou une vidéo préparée sur le PC (il y en a même une pour Halloween !).
- **Balise autonome** : cycle mire / caméra / pause, démarrage automatique à la mise sous tension, une **LED d'état** qui
  « chauffe » avec la puissance.
- **Réglage depuis le téléphone** en 11 langues, hors émission.
- Firmware, outils, documentation et rapports d'essais **libres** (GPL-3.0).

## Ce qu'il faut

- Une carte ESP32-S3 N16R8 avec caméra (testée : diymore ESP32-S3-CAM, brochage Freenove).
- Une antenne 13 cm et un **filtre passe-bande** ; un amplificateur linéaire pour les plus grandes distances.
- Un récepteur DVB-S2 (Minitiouner, Portsdown, récepteurs satellite compatibles).

## Collaborons !

Le projet vient de naître et grandira avec la communauté radioamateur. Toute contribution est la bienvenue :

- **essais en l'air** : distance, MER, récepteur et antennes utilisés, photos de la réception ;
- **signalements de problèmes** et **demandes d'amélioration** ;
- idées et réalisations : amplificateurs et filtres, transverters pour d'autres bandes, autres cartes ESP32-S3, débits
  symboles plus élevés ;
- **relecture des traductions** de la page web et des documents.

Dépôt, signalements et discussions : **https://github.com/Photoluis/esp32s3-cam-pdx-datv-beacon** — contact : **iz7pdx@gmail.com**

*Avertissement : réservé aux radioamateurs titulaires d'une licence, sur les fréquences et avec les puissances autorisées
par celle-ci, toujours avec un filtre adapté en sortie.*
