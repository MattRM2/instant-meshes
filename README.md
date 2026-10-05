# Instant Meshes
[![Build Status](https://travis-ci.org/wjakob/instant-meshes.svg?branch=master)](https://travis-ci.org/wjakob/instant-meshes)
[![Build status](https://ci.appveyor.com/api/projects/status/dm4kqxhin5uxiey0/branch/master?svg=true)](https://ci.appveyor.com/project/wjakob/instant-meshes/branch/master)

<img width="170" height="166" src="https://github.com/wjakob/instant-meshes/raw/master/resources/icon.png">

This repository contains the interactive meshing software developed as part of the publication

> **Instant Field-Aligned Meshes**<br/>
> Wenzel Jakob, Marco Tarini, Daniele Panozzo, Olga Sorkine-Hornung<br/>
> In *ACM Transactions on Graphics (Proceedings of SIGGRAPH Asia 2015)*<br/>
> [PDF](http://igl.ethz.ch/projects/instant-meshes/instant-meshes-SA-2015-jakob-et-al.pdf),
> [Video](https://www.youtube.com/watch?v=U6wtw6W4x3I),
> [Project page](http://igl.ethz.ch/projects/instant-meshes/)


## Ce fork : support natif d'Alembic (.abc) et d'USD

Fork de [wjakob/instant-meshes](https://github.com/wjakob/instant-meshes) qui
ajoute la lecture et l'écriture des fichiers Alembic **sans aucune
dépendance** (pas de bibliothèque Alembic, pas d'Imath) : le format Ogawa, les
maillages polygonaux, les transformations, les instances et la chaîne
d'empreintes (MurmurHash3 / SpookyHash) sont implémentés dans `src/ogawa.*`,
`src/abc*.cpp`. Géométrie polygonale uniquement, pas d'animation (un fichier
animé est lu à sa première image).

USD (`.usd`, `.usda`, `.usdc`, `.usdz`) est lu et écrit de la même façon, **sans la
bibliothèque USD** (`src/usd*.cpp`) : texte, binaire Crate (versions 0.2 à
0.13, compressé ou non) et paquet usdz, vérifiés valeur par valeur contre la
bibliothèque de Pixar. Sur les 2 440 fichiers de test d'OpenUSD lui-même
(tous les recoins du format, Crate 0.0.1 à 0.11) : 2 214 calques lus comme
Pixar les lit, 1 731 de ses 1 740 maillages composés à l'identique, et aucun
fichier ne fait planter ni bloquer le lecteur (`tests/check_openusd_corpus.py`).

### Ligne de commande

```
InstantMeshes.exe scene.abc -o scene_retopo.abc -f 75%
```

| Option | Rôle |
|---|---|
| `-o fichier.abc` | Sortie Alembic (aussi `.obj` / `.ply` / `.usda` / `.usdc` / `.usdz`) ; peut être le fichier d'entrée (remplacement atomique) |
| `-f 75%` | Objectif en pourcentage des polygones d'origine (tels qu'affichés dans Blender) ; `-f 5000` = nombre de faces |
| `--list` | Liste les maillages d'un `.abc` (chemin, faces, sommets, animé / instancié) ou les objets d'un `.obj` ou d'un fichier USD |
| `-m <nom>=<cible>` | Remaille séparément les maillages désignés et les réinjecte dans une copie du fichier (`.abc` ou `.obj`), ou dans un calque `.usda` / `.usdc` posé sur un fichier USD |
| `--proxy` | USD : garde les maillages et ajoute leur copie remaillée comme **proxy** (purpose `proxy`, `proxyPrim` sur l'original passé en `render`, matériau de l'original, UV transférées par défaut) dans un calque de proxies `<sortie>_proxy.usda` / `.usdc`, **référencé par la scène** |
| `--others <cible>` | Remaille aussi tous les autres maillages ; sans cette option ils sont recopiés intacts |
| `--sort asc\|desc`, `--top <n>` | Avec `--list` : tri par nombre de faces croissant / décroissant, et seulement les n premiers (`--sort desc --top 10` = les 10 objets les plus lourds) |
| `--dry-run` | Affiche le plan de `-m` / `--others` sans rien calculer ni écrire |
| `--uv transfer` | Transfère toutes les cartes UV de l'original sur le maillage remaillé, îlot par îlot (aucune face étirée sur une couture), en fichier entier comme en mode par objet ; sortie `.obj`, `.abc` ou USD (`--uv none` par défaut) |
| `--uv unwrap` | Nouvelles UV pour le maillage remaillé (xatlas, intégré) : îlots aplatis avec peu de déformation et rangés dans le carré [0, 1], polygones jamais coupés, aucun îlot en miroir ni recouvrement ; une carte `UVMap` |
| `--keep-border` | Replace le bord libre remaillé exactement sur le bord d'origine (coins et courbes compris) : des objets qui se touchent, comme des plaques de sol, restent jointifs après un remaillage séparé. Implique `-b` ; sortie `.obj`, `.abc` ou USD |
| `--progress` | Affiche un bloc de progression bien visible (3 lignes) avant le premier maillage et après chaque maillage traité : pourcentage pondéré par les faces d'entrée, barre, maillages faits / total, temps écoulé et temps restant estimé |
| `--skip-failed` | Un maillage impossible à remailler (ex. aucune face pour une cible trop petite) est recopié intact au lieu de tout arrêter ; la liste des maillages sautés est affichée à la fin |

Exemple, par objet :

```
InstantMeshes.exe scene.abc -o scene_retopo.abc -m "MeshA=75%" -m "MeshB=85%" --others 25%
```

- `<nom>` : nom d'objet (`MeshA`) ou chemin depuis la racine (`Props/MeshA`, le
  `/` initial est facultatif). Jokers `*` et `?` (`"Mesh*=75%"`,
  `"Props/*=60%"`) ; sensible à la casse. Un parent désigne chacun des
  maillages en dessous.
- Si plusieurs `-m` désignent le même maillage, **le dernier l'emporte** :
  écrire le cas général d'abord, les exceptions ensuite.
- `<cible>` : `75%` (des polygones de ce maillage) ou `5000` faces.
- Tout est vérifié avant le premier calcul (règle sans correspondance, objet
  animé ou instancié, sortie non `.abc`). Si un maillage échoue, rien n'est
  écrit.
- Mettre les jokers entre guillemets (sous Git Bash / Linux / macOS le shell
  les développerait).

Pour un maillage remaillé : nom, transformation du parent et propriétés
utilisateur sont conservés ; UV, normales et attributs par sommet / face sont
retirés (ils ne correspondent plus à la topologie) ; un matériau unique (face
set) est reconstruit sur toutes les faces, plusieurs matériaux sont retirés.
Le reste du fichier (autres objets, caméras, courbes, animations,
métadonnées) est recopié bloc pour bloc.

Scènes OBJ : même mode par objet (`-m`, `--others`, `--list`, `--dry-run`),
sortie `.obj`. Les objets sont les blocs `o`, ou les groupes `g` sans `o`. Les
objets non visés gardent leurs lignes telles quelles (positions, UV, normales,
matériaux), seuls leurs indices de faces sont renumérotés ; un objet remaillé
garde son matériau le plus utilisé (un OBJ ne peut pas laisser un objet sans
matériau).

Scènes USD : même mode par objet, mais la sortie est un **calque** (`.usda` ou `.usdc`)
léger qui charge l'original en sous-calque (`subLayers`) et ne surcharge
(`over`) que les maillages remaillés ; le fichier d'origine n'est jamais
modifié. Ouvrir le calque dans Blender, Houdini, Maya ou usdview.

```
InstantMeshes.exe asset.usdc -o asset_retopo.usda -m "Hero*=50%" --uv transfer
```

- Lu : les prims `Mesh` définis (`def`) du fichier, placés par `xformOpOrder`
  (toutes les opérations, `!invert!`, `!resetXformStack!`), première
  image si animé, UV en primvars `texCoord2f` (ou `st` / `uv`).
- Écrit pour un maillage remaillé : points (dans l'espace du maillage),
  faces, `extent`, UV si `--uv` ; normales, primvars non constants,
  plis et trous bloqués (`= None`) ; `GeomSubset` désactivés et maillage lié
  à leur matériau le plus utilisé. Métadonnées de la scène recopiées.
- Le fichier est **composé** comme le fait USD : sous-calques, références,
  payloads, variantes sélectionnées, inherits, specializes. Un maillage venu
  d'une référence peut être remaillé ou recevoir un proxy ; les fichiers
  référencés ne sont jamais modifiés. Non composés : décalages temporels,
  relocates, value clips.
- Sorties binaires : `.usdc` (Crate 0.8.0) et `.usdz` (paquet), environ 2,5 fois
  plus légères que le texte ; le vérificateur de conformité de Pixar ne leur
  trouve aucune erreur. Un calque posé sur une scène est `.usda` ou `.usdc`
  (un `.usdz` devrait contenir la scène).

Proxies USD (`--proxy`) : les maillages visés par `-m` / `--others` sont
gardés, leur copie remaillée est ajoutée comme proxy (ce que les viewports
affichent, le rendu garde l'original). Les proxies vont dans un **calque à
part**, et c'est la **scène qui le référence** (comme en production) : on
continue d'ouvrir la scène, qui amène ses proxies. Trois passes donnent trois
calques de proxies, tous référencés par la même scène.

```
# -o nomme la scène : elle est modifiée sur place
InstantMeshes.exe asset.usdc -o asset.usdc --proxy --others 5%
# un autre nom : une copie de la scène, comme un « Enregistrer sous »
InstantMeshes.exe asset.usdc -o asset_v2.usdc --proxy -m "Hero=10%" --others 3%
```

- Sur place, la **seule modification** de la scène est la référence
  (`prepend references = @./asset_proxy.usdc@</Asset>` sur la prim racine) :
  dans un `.usdc` les nouvelles données sont ajoutées à la fin, tous les
  autres octets restent en place ; dans un `.usda` la référence est écrite
  dans le texte, le reste intact. Une copie garde le format de la scène
  (`.usda`, `.usdc` ou `.usd`).
- Un paquet **`.usdz` reste un paquet** : son calque racine reçoit la
  référence, le calque de proxies va **dedans** (`asset_proxy.usdc`, à côté
  du calque racine), ses autres fichiers (sous-calques, textures) sont
  gardés tels quels. Sur place ou en copie (`-o` un autre `.usdz`) ; le
  vérificateur de conformité de Pixar ne trouve aucune erreur. Les calques
  d'un paquet sont lus sur place (`scene.usdz[parts/asset.usdc]`).
- Le calque de proxies est `<sortie>_proxy`, à côté de la sortie, dans son
  format (`asset_proxy.usdc`). Il contient les proxies et ce que gagnent les
  maillages (purpose, `proxyPrim`), et ne charge rien lui-même. Une deuxième
  passe écrit `asset_proxy2.usdc`, référencé lui aussi.
- Le proxy reprend le matériau de l'original. Un matériau hors de la prim
  racine (`/materials`, comme en sort Solaris) passe par un relais,
  `/World/proxy_materials/<nom>`, qui le **référence** : même réseau, mêmes
  textures, ses modifications suivies.

- Convention `geo/render` : `/Asset/geo/render/Body` reçoit
  `/Asset/geo/proxy/Body`, même hiérarchie, transformations recopiées
  (animation comprise). Jamais sous le scope `render`, que Blender ignore en
  bloc quand on importe les proxies.
- Sinon : un frère `<nom>_proxy` avec la transformation du maillage.
- Les maillages `proxy` / `guide` n'en reçoivent pas ; un proxy qui existe
  déjà est une erreur, détectée avant tout calcul (pour le refaire, retirer
  sa référence). Un maillage à la racine (`/Hero`) n'en reçoit pas : il doit
  être sous une prim racine (`/World/Hero`). `--dry-run` montre où chaque
  proxy ira et le calque qui les contient.
- Blender importe les maillages de rendu par défaut : cocher **Proxy** dans les
  options d'import USD pour voir les proxies.

D'un OBJ à un asset USD avec son proxy, en deux commandes : conversion au
même nombre de polygones (100 % : remaillé, autant de faces environ) avec de
nouvelles UV, puis un proxy à 10 % qui reprend ces UV. La première commande
fusionne tous les objets de l'OBJ en un seul maillage.

```
InstantMeshes.exe model.obj -o model.usda -f 100% --uv unwrap
InstantMeshes.exe model.usda -o model.usda --proxy --others 10%
```

Instances : en mode par maillage, une géométrie instanciée est **une seule
entrée** (`--list` : `(instanced x250)`), remaillée **une seule fois**, et
toutes ses instances montrent le résultat ; l'instanciation est conservée.
`-m` accepte n'importe laquelle de ses apparitions.

- **Alembic** : la géométrie est remplacée sur l'objet source, que toutes les
  instances (`.instanceSource`) reprennent.
- **USD, PointInstancer** : le maillage du prototype est remplacé sur place.
- **USD, prims `instanceable`** : USD ignore toute modification sous une
  instance ; le nouveau maillage va dans une **classe** (`_IM_...`) dont
  chaque instance **hérite** (`inherits`), le fichier référencé n'est pas
  touché. Les instances partagent toujours un seul prototype.
- **Variantes** : d'autres sélections = d'autres prototypes, listés à part ;
  à géométrie identique (variantes de matériau), remaillée une fois.
- `--proxy` : le proxy va dans le prototype, chaque instance l'a.
- **Instances imbriquées** (USD) : listées, pas remaillées.
- **Mode fichier entier** (`-f`, `-s`, `-v` sans `-m`) : un seul maillage, donc
  pas d'instanciation possible. En Alembic, chaque instance est placée et
  fusionnée ; en USD, les maillages instanciés sont **ignorés** (un
  avertissement les compte). Pour garder l'instanciation : le mode par
  maillage.

Mémoire, en mode par objet : un seul maillage est remaillé à la fois (environ
800 octets par sommet d'entrée), le pic est donc celui du plus gros objet
remaillé, pas de la scène. Un Alembic est lu à la demande ; un OBJ est lu une
seule fois et gardé en mémoire à environ 3 fois sa taille. Les maillages
remaillés attendent l'écriture finale dans un fichier temporaire à côté de la
sortie (`.spool.tmp`, supprimé à la fin). Un objet dont l'entrée doit être
subdivisée avant le remaillage (le plus coûteux) est signalé dans le log.

Précision de `-f N%` : environ ±3 % sur des maillages réels, moins précis en
dessous de quelques centaines de polygones.

**La forme d'abord** : après chaque remaillage, la surface d'origine est
comparée au résultat. S'il a perdu des parties (tubes, fils, cadres, panneaux
plus fins que les arêtes, dont les faces ont fusionné), le maillage est
refait avec deux fois plus de faces, jusqu'à 5 essais, quitte à dépasser
l'objectif (le log le dit). Un maillage qui ne garde sa forme qu'avec autant
de faces qu'il en a (props low-poly, panneaux fins) est **gardé tel quel** et
listé en fin de passe ; pas de proxy pour lui. Mesuré sur le Kitchen Set de
Pixar : 842 proxies sur 859 sans partie manquante.

### Interface : Outliner et projets (`.imd`)

- **Menu File** : New, Open (scènes `.abc` `.obj` `.usd*`, projets `.imd`, maillages), Open recent, Save,
  Save as, Import legacy state, Export mesh, Write scene, Quit, avec Ctrl+N / O / S / Maj+S / Q. Un
  fichier lâché sur la fenêtre s'ouvre aussi.
- **Outliner** (à droite) : les maillages de la scène en arborescence. Cases à cocher, sélection,
  tri, filtre (`Rock`, `*Rock*`, `Props/*`). Double-clic : le maillage s'ouvre seul dans la vue.
  Par maillage : faces, cible propre (comme `-m`, en orange) ou par défaut (`--others`), état
  (done, failed, stale).
- **Process checked** remaille les maillages cochés en tâche de fond (barre de progression,
  annulation). **Use viewport result** garde un maillage retouché à la main. Le bandeau en haut
  de la vue indique ce qu'elle montre : `WHOLE SCENE` (scène entière, tous les maillages fusionnés)
  ou `MESH: nom (alone)`. Extrait de la scène entière, le résultat est redécoupé par maillage
  (chaque face va au maillage le plus proche) ; extrait d'un maillage seul, il reste à ce
  maillage. **Write scene**
  écrit la scène. **Copy command line** donne la même commande pour la ferme.
- **Projets `.imd`** (File > Save) : la scène (référencée, pas copiée), les réglages, la cible et
  l'état de chaque maillage, les résultats déjà calculés et les traits faits à la main. Si la scène
  a bougé, le projet la cherche à côté de lui puis te la demande ; si elle a changé, les maillages
  modifiés passent en « stale ».
- **Ligne de commande** : `--save-imd job.imd` sauve un plan (`-m` / `--others`, avec ou sans
  `--dry-run`) ; `InstantMeshes.exe job.imd -o scene_retopo.abc` l'exécute (ce qui est déjà fait
  n'est pas recalculé) ; `--register-imd` (ou File > Open .imd files with Instant Meshes) ouvre les
  `.imd` d'un double-clic, avec leur icône (pour ton utilisateur, sans droits administrateur).

### Compiler (Windows)

Double-cliquer `build_windows.bat` (Visual Studio 2022 requis ; le script
utilise le CMake 3.x fourni avec Visual Studio, CMake 4 étant incompatible avec
le GLFW embarqué). Il lance ensuite les tests : `im_tests` (unitaires, dont le
fuzzing des lecteurs) et `tests/test_cli.py` (ligne de commande, si Python est
installé). Outils : `build\Release\abc_dump.exe [--verify] fichier.abc`, et
les scripts Blender de `tests/`.

##### In commercial software

Since version 10.2, Modo uses the Instant Meshes algorithm to implement its
automatic retopology feature. An interview discussing this technique and more
recent projects is available [here](https://www.foundry.com/trends/design-visualisation/mitsuba-renderer-instant-meshes).

## Screenshot

![Instant Meshes 2.4: the Matt Dark interface and the Outliner](docs/cover.png)

## Pre-compiled binaries

The following binaries (Intel, 64 bit) are automatically generated from the latest GitHub revision.

> [Microsoft Windows](https://instant-meshes.s3.eu-central-1.amazonaws.com/Release/instant-meshes-windows.zip)<br/>
> [Mac OS X](https://instant-meshes.s3.eu-central-1.amazonaws.com/instant-meshes-macos.zip)<br/>
> [Linux](https://instant-meshes.s3.eu-central-1.amazonaws.com/instant-meshes-linux.zip)

Please also fetch the following dataset ZIP file and extract it so that the
``datasets`` folder is in the same directory as ``Instant Meshes``, ``Instant Meshes.app``,
or ``Instant Meshes.exe``.

> [Datasets](https://instant-meshes.s3.eu-central-1.amazonaws.com/instant-meshes-datasets.zip)

Note: On Linux, Instant Meshes relies on the program ``zenity``, which must be installed.

## Compiling

Compiling from scratch requires CMake and a recent version of XCode on Mac,
Visual Studio 2015 on Windows, and GCC on Linux. 

On MacOS, compiling should be as simple as

    git clone --recursive https://github.com/wjakob/instant-meshes
    cd instant-meshes
    cmake .
    make -j 4

To build on Linux, please install the prerequisites ``libxrandr-dev``,
``libxinerama-dev``, ``libxcursor-dev``, and ``libxi-dev`` and then use the
same sequence of commands shown above for MacOS.

On Windows, open the generated file ``InstantMeshes.sln`` after step 3 and proceed building as usual from within Visual Studio.

## Usage

To get started, launch the binary and select a dataset using the "Open mesh" button on the top left (the application must be located in the same directory as the 'datasets' folder, otherwise the panel will be empty).

The standard workflow is to solve for an orientation field (first blue button) and a position field (second blue button) in sequence, after which the 'Export mesh' button becomes active. Many user interface elements display a descriptive message when hovering the mouse cursor above for a second.

A range of additional information about the input mesh, the computed fields,
and the output mesh can be visualized using the check boxes accessible via the
'Advanced' panel.

Clicking the left mouse button and dragging rotates the object; right-dragging
(or shift+left-dragging) translates, and the mouse wheel zooms. The fields can also be manipulated using brush tools that are accessible by clicking the first icon in each 'Tool' row.
