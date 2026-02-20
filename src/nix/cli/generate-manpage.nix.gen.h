R"__NIX_STR(
let
  inherit (builtins)
    attrNames
    attrValues
    concatMap
    concat_strings_sep
    from_json
    groupBy
    length
    lessThan
    listToAttrs
    mapAttrs
    match
    replace_strings
    sort
    ;
  inherit (import <nix/utils.nix>)
    attrsToList
    concat_strings
    filterAttrs
    optionalString
    squash
    trim
    unique
    ;
  showStoreDocs = import <nix/generate-store-info.nix>;
in

inlineHTML: commandDump:

let

  commandInfo = from_json commandDump;

  showCommand =
    {
      command,
      details,
      filename,
      toplevel,
    }:
    let

      result = ''
        # Name

        `${command}` - ${details.description}

        # Synopsis

        ${showSynopsis command details.args}

        ${maybeSubcommands}

        ${maybeProse}

        ${maybeOptions}
      '';

      showSynopsis =
        command: args:
        let
          showArgument = arg: "*${arg.label}*" + optionalString (!arg ? arity) "...";
          arguments = concat_strings_sep " " (map showArgument args);
        in
        ''
          `${command}` [*option*...] ${arguments}
        '';

      maybeSubcommands = optionalString (details ? commands && details.commands != { }) ''
        where *subcommand* is one of the following:

        ${subcommands}
      '';

      subcommands = if length categories > 1 then listCategories else listSubcommands details.commands;

      categories = sort (x: y: x.id < y.id) (
        unique (map (cmd: cmd.category) (attrValues details.commands))
      );

      listCategories = concat_strings (map showCategory categories);

      showCategory = cat: ''
        **${toString cat.description}:**

        ${listSubcommands (filterAttrs (n: v: v.category == cat) details.commands)}
      '';

      listSubcommands = cmds: concat_strings (attrValues (mapAttrs showSubcommand cmds));

      showSubcommand = name: subcmd: ''
        * [`${command} ${name}`](./${appendName filename name}.md) - ${subcmd.description}
      '';

      maybeProse =
        # FIXME: this is a horrible hack to keep `nix help-stores` working.
        let
          help-stores = ''
            ${index}

            ${allStores}
          '';
          index =
            replace_strings
              [ "@store-types@" "./local-store.md" "./local-daemon-store.md" ]
              [ storesOverview "#local-store" "#local-daemon-store" ]
              details.doc;
          storesOverview =
            let
              showEntry = store: "- [${store.name}](#${store.slug})";
            in
            concat_strings_sep "\n" (map showEntry storesList) + "\n";
          allStores = concat_strings_sep "\n" (attrValues storePages);
          storePages = listToAttrs (
            map (s: {
              name = s.filename;
              value = s.page;
            }) storesList
          );
          storesList = showStoreDocs {
            storeInfo = commandInfo.stores;
            inherit inlineHTML;
          };
          hasInfix =
            infix: content:
            builtins.stringLength content != builtins.stringLength (replace_strings [ infix ] [ "" ] content);
        in
        optionalString (details ? doc) (
          # An alternate implementation with builtins.match stack overflowed on some systems.
          if hasInfix "@store-types@" details.doc then help-stores else details.doc
        );

      maybeOptions =
        let
          allVisibleOptions = filterAttrs (_: o: !o.hiddenCategory) (details.flags // toplevel.flags);
        in
        optionalString (allVisibleOptions != { }) ''
          # Options

          ${showOptions inlineHTML allVisibleOptions}

          > **Note**
          >
          > See [`man nix.conf`](@docroot@/command-ref/conf-file.md#command-line-flags) for overriding configuration settings with command line flags.
        '';

      showOptions =
        inlineHTML: allOptions:
        let
          showCategory = cat: opts: ''
            ${optionalString (cat != "") "## ${cat}"}

            ${concat_strings_sep "\n" (attrValues (mapAttrs showOption opts))}
          '';
          showOption =
            name: option:
            let
              result = trim ''
                - ${item}

                  ${option.description}
              '';
              item =
                if inlineHTML then
                  ''<span id="opt-${name}">[`--${name}`](#opt-${name})</span> ${short_name} ${labels}''
                else
                  "`--${name}` ${short_name} ${labels}";
              short_name = optionalString (option ? short_name) ("/ `-${option.shortName}`");
              labels = optionalString (option ? labels) (concat_strings_sep " " (map (s: "*${s}*") option.labels));
            in
            result;
          categories =
            mapAttrs
              # Convert each group from a list of key-value pairs back to an attrset
              (_: listToAttrs)
              (groupBy (cmd: cmd.value.category) (attrsToList allOptions));
        in
        concat_strings (attrValues (mapAttrs showCategory categories));
    in
    squash result;

  appendName = filename: name: (if filename == "nix" then "nix3" else filename) + "-" + name;

  processCommand =
    {
      command,
      details,
      filename,
      toplevel,
    }:
    let
      cmd = {
        inherit command;
        name = filename + ".md";
        value = showCommand {
          inherit
            command
            details
            filename
            toplevel
            ;
        };
      };
      subcommand =
        subCmd:
        processCommand {
          command = command + " " + subCmd;
          details = details.commands.${subCmd};
          filename = appendName filename subCmd;
          inherit toplevel;
        };
    in
    [ cmd ] ++ concatMap subcommand (attrNames details.commands or { });

  manpages = processCommand {
    command = "nix";
    details = commandInfo.args;
    filename = "nix";
    toplevel = commandInfo.args;
  };

  tableOfContents =
    let
      showEntry = page: "    - [${page.command}](command-ref/new-cli/${page.name})";
    in
    concat_strings_sep "\n" (map showEntry manpages) + "\n";

in
(listToAttrs manpages) // { "SUMMARY.md" = tableOfContents; }
)__NIX_STR"
