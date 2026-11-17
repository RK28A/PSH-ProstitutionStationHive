# Ce fork : performances, mesures et état des lieux

Ce dépôt est un fork de [KytyPS5](https://github.com/KytyPS5/KytyPS5) pris au commit amont
`d45b0865`. L'historique de l'émulateur lui-même reste dans le dépôt amont. Ce document dit ce
qui a été changé, ce que chaque changement rapporte **mesures à l'appui**, et ce qui ne l'a pas
été.

## Ce qu'il faut savoir avant tout

Aucun jeu PS5 n'a pu être lancé pendant ce travail : il a été fait sur une machine sans carte
graphique ni jeu. Chaque changement de `main` est donc validé par les tests du projet et par un
banc de mesure, pas par un compteur d'images dans un jeu. **Rien ici ne garantit 30 ou 60 images
par seconde dans un jeu donné** : cela dépend du jeu, du processeur, de la carte graphique et de
son pilote. Les gains ci-dessous sont réels là où ils sont mesurés ; leur effet dans un jeu reste
à constater.

## Changements sur `main`

| Changement | Mesure | Fichiers |
| --- | --- | --- |
| Les threads en attente d'un mutex du jeu guettent leur tour 50 µs avant de s'endormir, sans changer l'ordre FIFO d'origine | 2 threads sur un mutex : 80 000 → 0,6 à 2,9 millions de verrouillages/s ; 4 threads : 76 000 → 0,55 à 0,75 million | `src/kernel/pthread.cpp` |
| Le moteur de rendu n'interroge plus le pilote Vulkan à chaque draw quand aucune opération différée ne peut être prête | `PopPendingOperations` : 2,8 % → 0,9 % du temps d'un draw sur lavapipe ; l'appel supprimé est un appel système sur les pilotes Mesa | `src/graphics/host_gpu/renderer/commandScheduler.cpp`, `pipeline/descriptorHeap.cpp` |
| Chaque thread garde ses dernières recherches de métadonnées de shader au lieu de prendre un verrou global à chaque étage de chaque draw | `ShaderGetMappedData` : 3,3 % → 0,7 % du temps d'un draw sur lavapipe | `src/graphics/shader/shader.cpp` |
| Timers précis sous Linux (`PR_SET_TIMERSLACK`) | un sommeil de 100 µs durait 160 µs, il en dure 114 ; 50 µs : 110 → 63 | `src/common/threads.cpp` |
| Reconfigurer CMake dans un dossier de build existant n'échoue plus (le patch ZArchive n'est appliqué qu'une fois) | deux `cmake` de suite réussissent | `3rdparty/CMakeLists.txt`, `3rdparty/patches/apply_once.cmake` |

Le premier point est le plus important. Avant, dès que deux threads d'un jeu se partageaient un
mutex, chaque passage de relais attendait le réveil d'un thread endormi, et le débit du mutex était
divisé par 300 quelle que soit la brièveté de la section critique.

La CI du dépôt compile ces commits et passe ses tests sur Windows, macOS et Linux.

## Outils de mesure et de test ajoutés

Les tests GPU du projet exigeaient une carte graphique. Ils tournent maintenant sur le rasteriseur
logiciel lavapipe, grâce à une petite couche Vulkan utilisée par les tests seulement
(`tests/software_gpu_layer/`) : l'émulateur ne la charge jamais.

```bash
cmake --build _Build/linux-no-qt --target shader_recompiler_compute_tests kyty_software_gpu_layer
```

```bash
tests/run_gpu_tests.sh _Build/linux-no-qt
```

Le script lance chaque sélecteur de `shader_recompiler_compute_tests` dans son propre processus et
compare le résultat à `tests/software_gpu_layer/expected_failures.txt`. Avec Mesa 25.2.8,
103 sélecteurs (409 cas) passent ; les 37 autres échouent à cause de lavapipe (pas de MSAA 2x, pas
de minLod de vue, une erreur LLVM sur un geometry shader) et sont listés pour qu'un changement du
moteur de rendu se voie comme un écart à cette liste. Il faut `xvfb` et `mesa-vulkan-drivers`.

Trois bancs de mesure :

```bash
shader_recompiler_compute_tests --draw-bench 100000
```

donne le temps processeur par draw sur le thread qui enregistre les commandes, pour sept cas
(état inchangé, shader, couleur de mélange, viewport, cible ou texture qui alternent). À lancer
avec les mêmes variables d'environnement que le script ci-dessus.

```bash
kernel_file_system_tests --mutex-bench 500
```

donne le débit d'un mutex du jeu partagé par 1, 2, 4 et 8 threads, et vérifie l'exclusion
mutuelle.

```bash
shader_recompiler_compute_tests --fault-bench 4096
```

donne le nombre de fautes et le temps pris pour écrire des pages suivies par le cache de buffers.

Ordres de grandeur relevés sur lavapipe : environ 1 µs de processeur par draw sans texture, 3 µs
avec une texture. Dans ce temps, la résolution des shaders à chaque draw
(`PipelineCache::GetGraphicsPrograms`) pèse environ 30 %, celle des cibles de rendu 10 %, la
recherche du pipeline 7 %.

## Branches expérimentales

Deux changements touchent des mécanismes trop sensibles pour aller sur `main` sans avoir été
essayés sur des jeux. Chacun a sa branche, construite par la CI comme `main` ; les deux passent les
mêmes tests que `main`.

**`experimental/fault-ahead`** s'attaque au coût des fautes de page du suivi mémoire GPU. Chaque
page de 4 Ko qu'un jeu écrit dans une zone suivie coûte une faute d'environ 3 µs sous Linux,
presque entièrement dans le noyau. Quand les fautes d'un thread se suivent page après page, la
branche libère aussi les pages suivantes, deux fois plus à chaque fois, jusqu'à 64. Mesuré avec
`shader_recompiler_compute_tests --fault-bench` :

| Pages écrites dans l'ordre | `main` | `experimental/fault-ahead` |
| --- | --- | --- |
| 256 (1 Mo) | 256 fautes, 1,03 ms | 10 fautes, 0,064 ms |
| 4096 (16 Mo) | 4096 fautes, 10,7 ms | 70 fautes, 0,69 ms |
| 16384 (64 Mo) | 16384 fautes, 43,3 ms | 262 fautes, 2,19 ms |

Le risque : une page libérée que le jeu n'écrit finalement pas est renvoyée au GPU à l'identique,
ce qui coûte un envoi inutile, et le chemin modifié est au cœur de la synchronisation entre le
processeur et le GPU.

**`experimental/mutex-barging`** change **quel** thread obtient un mutex disputé : un mutex libéré
revient au premier thread qui le demande, comme sur la console, au lieu d'être réservé au premier
de la file ; un thread dépassé depuis 1 ms le reçoit quand même directement. Avec plus de threads
que de cœurs, le débit passe de 71 000 à environ 2 millions de verrouillages/s. À quatre threads
sur quatre cœurs, le résultat n'est pas meilleur que `main`. L'amont a choisi l'ordre FIFO strict
en octobre 2026 ; ce changement peut donc casser un jeu qui en dépend.

## Obtenir un build

Chaque push sur `main` lance le workflow `Build and Release KytyPS5`, qui publie les builds
Windows, macOS et Linux comme artefacts de l'exécution (onglet Actions du dépôt). Ces builds
utilisent l'optimisation à l'édition de liens. Pour un build local équivalent, ajoutez
`-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON` à la commande `cmake` du README.

## Pistes repérées, non réalisées

Classées par gain attendu. Aucune n'est faite.

1. **Résolution des shaders par draw.** `GetGraphicsPrograms` reconstruit la clé de chaque étage
   et reparcourt les tables de ressources à chaque draw. Un cache du dernier résultat demande une
   clé d'invalidation exacte, y compris la mémoire du jeu que le parcours lit.
2. **Chemin rapide des mutex sans verrou interne.** Verrouiller puis déverrouiller un mutex libre
   prend 37 ns et passe deux fois par un `std::mutex` ; un mot atomique diviserait ce coût.
3. **Sommeils courts sous Windows.** Les timers haute résolution y ont un grain d'environ 0,5 ms,
   donc un `usleep(100)` du jeu dure plusieurs fois trop longtemps. Le corriger demande une
   attente active dont le coût processeur ne peut se régler que sur des jeux.
4. **Ne pas réémettre des descripteurs identiques.** Étudié et écarté : les constantes par objet
   changent à chaque draw, le cas ne se présenterait presque jamais.

## La liste de 500 points

État par section, établi en lisant le code : « présent » veut dire que KytyPS5 le fait déjà.
Ce n'est pas une vérification point par point.

| Points | Sujet | État |
| --- | --- | --- |
| 1–20 | Shaders et pipelines | Présents : cache de pipelines sur disque, préchauffage au démarrage réparti sur les cœurs, déduplication par empreinte du code, IR avec passes, extraction des shaders. Absents : compilation asynchrone, ubershader de repli, `graphics_pipeline_library`, `shader_object` |
| 21–40 | Rendu et backend | Présents : thread GPU dédié, rendu dynamique, tas d'images bindless, états dynamiques non réémis, détuilage GPU, FSR/DLSS, HDR10, timeline semaphores, validation et RenderDoc. Absents : enregistrement parallèle des command buffers, backends Metal ou DX12 natifs |
| 41–60 | Mémoire | Présent : suivi des écritures par protection de pages. **Branche expérimentale : regroupement des fautes d'écriture (44, 45).** Absents : huge pages, `userfaultfd`/`GetWriteWatch`, import par `external_memory_host` |
| 61–80 | CPU et threads | Présents : un thread hôte par thread du jeu, patch unique des instructions spéciales, émulation des instructions absentes, Rosetta 2. **Faits ici : attente avant sommeil des mutex (78), timers précis sous Linux (67).** Absents : priorités de threads, placement sur les cœurs performants |
| 81–100 | HLE système | Présents : trophées, plusieurs utilisateurs, clavier virtuel, dialogues, décodeur AJM. Non examinés en détail |
| 101–120 | Performance et profilage | Présents : Tracy, overlay, LTO en CI, mode mailbox. **Faits ici : bancs de mesure reproductibles (115), un appel pilote en moins par draw.** Absent : PGO |
| 121–160 | Stabilité, tests, CI | **Faits ici : tests GPU sur lavapipe en local (302), reconfiguration CMake réparée.** Non fait : lancer ces tests dans la CI (470), car la liste des échecs attendus dépend du processeur du runner |
| 161–200 | Audio, entrées, interface | Présents pour l'essentiel (DualSense, SDL3, lanceur, mises à jour). Non modifiés |
| 201–300, 371–390 | Écosystème, SharpEmu, RPCSX | Sans objet ici : ils concernent d'autres projets ou l'organisation d'une communauté |
| 301–370 | KytyPS5 et forks | 302 fait (voir ci-dessus). Le reste demande les forks cités ou des jeux pour valider |
| 391–450 | Recompilateur | Non modifié : chaque changement y exige une validation sur GPU réel |
| 451–500 | Pilotes et caches | Présents : signature du pilote dans la clé du cache, cache géré par l'application. Non modifiés |
