# ESP32-S3-CAM_PDX DATV Beacon

**Um transmissor DVB-S2 completo, com câmara, numa placa de poucas dezenas de euros**

*Luigi D'Arcangelo, IZ7PDX — projeto de código aberto, licença GPL-3.0*

![Receção da baliza com Portsdown 4 e Minitiouner: MER 26 dB](../img/test_mer_20261030/05_m9.6dB_MER26.0.jpg)

Uma única **placa ESP32-S3 com câmara** torna-se uma baliza de televisão digital amadora. O transmissor Wi-Fi do chip é
usado como **modulador I/Q**; tudo o resto — vídeo, multiplexador MPEG-TS, codificação **DVB-S2** segundo a norma ETSI
EN 302 307-1 e modulação QPSK — é calculado pela própria placa. Sem SDR, PC ou Raspberry Pi: alimentação, antena e
filtro.

## O que faz

- **DVB-S2 QPSK** a 333 kS/s, FEC de 1/2 a 9/10: recebido com **MER de 23–27 dB** em Portsdown 4 e Minitiouner.
- **2300–2450 MHz** diretamente; qualquer outra banda com um **misturador externo**: a frequência final aparece na imagem
  e no nome do serviço.
- **Câmara ao vivo** (H.264), câmara em **imagem sobre imagem** (PiP) no padrão de teste, padrão standard H.265 e
  **padrões personalizados**: uma foto do telemóvel ou um vídeo preparado no PC (há também um de Halloween!).
- **Baliza autónoma**: ciclo padrão / câmara / pausa, arranque automático ao ligar e um **LED de estado** que "aquece"
  com a potência.
- **Configuração pelo telemóvel** em 11 línguas, fora da transmissão.
- Firmware, ferramentas, documentação e relatórios de ensaio **livres** (GPL-3.0).

## O que é preciso

- Uma placa ESP32-S3 N16R8 com câmara (testada: diymore ESP32-S3-CAM, disposição Freenove).
- Uma antena para 13 cm e um **filtro passa-banda**; um amplificador linear para distâncias maiores.
- Um recetor DVB-S2 (Minitiouner, Portsdown, recetores de satélite compatíveis).

## Vamos colaborar!

O projeto acabou de nascer e vai crescer com a comunidade de radioamadores. Toda a contribuição é bem-vinda:

- **testes no ar**: distância, MER, recetor e antenas usados, fotos da receção;
- **relatórios de problemas** e **pedidos de melhoria**;
- ideias e montagens: amplificadores e filtros, transverters para outras bandas, outras placas ESP32-S3, taxas de símbolo
  mais altas;
- **revisão das traduções** da página web e dos documentos.

Repositório, relatórios e discussões: **{{REPO}}** — contacto: **iz7pdx@gmail.com**

*Aviso: apenas para radioamadores licenciados, nas frequências e com as potências permitidas pela sua licença, sempre com
um filtro adequado na saída.*
