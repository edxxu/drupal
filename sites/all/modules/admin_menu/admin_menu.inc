<?php

/**
 * @file
 * Menu builder functions for Administration menu.
 */

/**
 * Build the full administration menu tree from static and expanded dynamic items.
 *
 * @param $menu_name
 *   The menu name to use as base for the tree.
 */
function admin_menu_tree($menu_name) {
  // Get placeholder expansion arguments from hook_admin_menu_map()
  // implementations.
  module_load_include('inc', 'admin_menu', 'admin_menu.map');
  $expand_map = module_invoke_all('admin_menu_map');
  // Allow modules to alter the expansion map.
  drupal_alter('admin_menu_map', $expand_map);

  $new_map = array();
  $hidden = array();
  foreach ($expand_map as $path => $data) {
    // Convert named placeholders to anonymous placeholders, since the menu
    // system stores paths using anonymous placeholders.
    $replacements = array_fill_keys(array_keys($data['arguments'][0]), '%');
    $data['parent'] = strtr($data['parent'], $replacements);
    $new_map[strtr($path, $replacements)] = $data;

    // Collect paths to hide.
    if (isset($data['hide'])) {
      $hidden[strtr($data['hide'], $replacements)] = 1;
    }
  }
  $expand_map = $new_map;
  unset($new_map);

  // Retrieve dynamic menu link tree for the expansion mappings.
  // @todo Skip entire processing if initial $expand_map is empty and directly
  //   return $tree?
  if (!empty($expand_map)) {
    $tree_dynamic = admin_menu_tree_dynamic($expand_map);
  }
  else {
    $tree_dynamic = array();
  }

  // Merge local tasks with static menu tree.
  $tree = menu_tree_all_data($menu_name);
  admin_menu_merge_tree($tree, $tree_dynamic, array(), $hidden);

  return $tree;
}

/**
 * Load menu link trees for router paths containing dynamic arguments.
 *
 * @param $expand_map
 *   An array containing menu router path placeholder expansion argument
 *   mappings.
 *
 * @return
 *   An associative array whose keys are the parent paths of the menu router
 *   paths given in $expand_map as well as the parent paths of any child link
 *   deeper down the tree. The parent paths are used in admin_menu_merge_tree()
 *   to check whether anything needs to be merged.
 *
 * @see hook_admin_menu_map()
 */
function admin_menu_tree_dynamic(array $expand_map) {
  $p_columns = array();
  for ($i = 1; $i <= MENU_MAX_DEPTH; $i++) {
    $p_columns[] = 'p' . $i;
  }

  // Fetch p* columns for all router paths to expand.
  $router_paths = array_keys($expand_map);
  $plids = db_select('menu_links', 'ml')
    ->fields('ml', $p_columns)
    ->condition('router_path', $router_paths)
    ->execute()
    ->fetchAll(PDO::FETCH_ASSOC);

  // Unlikely, but possible.
  if (empty($plids)) {
    return array();
  }

  // Use queried plid columns to query sub-trees for the router paths.
  $query = db_select('menu_links', 'ml');
  $query->join('menu_router', 'm', 'ml.router_path = m.path');
  $query
    ->fields('ml')
    ->fields('m', array_diff(drupal_schema_fields_sql('menu_router'), drupal_schema_fields_sql('menu_links')));

  // The retrieved menu link trees have to be ordered by depth, so parents
  // always come before their children for the storage logic below.
  foreach ($p_columns as $column) {
    $query->orderBy($column, 'ASC');
  }

  $db_or = db_or();
  foreach ($plids as $path_plids) {
    $db_and = db_and();
    // plids with value 0 may be ignored.
    foreach (array_filter($path_plids) as $column => $plid) {
      $db_and->condition($column, $plid);
    }
    $db_or->condition($db_and);
  }
  $query->condition($db_or);
  $result = $query
    ->execute()
    ->fetchAllAssoc('mlid', PDO::FETCH_ASSOC);

  // Store dynamic links grouped by parent path for later merging and assign
  // placeholder expansion arguments.
  $tree_dynamic = array();
  foreach ($result as $mlid => $link) {
    // If contained in $expand_map, then this is a (first) parent, and we need
    // to store by the defined 'parent' path for later merging, as well as
    // provide the expansion map arguments to apply to the dynamic tree.
    if (isset($expand_map[$link['path']])) {
      $parent_path = $expand_map[$link['path']]['parent'];
      $link['expand_map'] = $expand_map[$link['path']]['arguments'];
    }
    // Otherwise, just store this link keyed by its parent path; the expand_map
    // is automatically derived from parent paths.
    else {
      $parent_path = $result[$link['plid']]['path'];
    }

    $tree_dynamic[$parent_path][] = $link;
  }

  return $tree_dynamic;
}

/**
 * Walk through the entire menu tree and merge in expanded dynamic menu links.
 *
 * @param &$tree
 *   A menu tree structure as returned by menu_tree_all_data().
 * @param $tree_dynamic
 *   A dynamic menu tree structure as returned by admin_menu_tree_dynamic().
 * @param $expand_map
 *   An array containing menu router path placeholder expansion argument
 *   mappings.
 * @param $hidden
 *   An array containing links to hide, keyed by path.
 *
 * @see hook_admin_menu_map()
 * @see admin_menu_tree_dynamic()
 * @see menu_tree_all_data()
 */
function admin_menu_merge_tree(array &$tree, array $tree_dynamic, array $expand_map, array $hidden) {
  foreach ($tree as $key => $data) {
    $path = $data['link']['router_path'];

    // Recurse into regular menu tree.
    if ($tree[$key]['below']) {
      admin_menu_merge_tree($tree[$key]['below'], $tree_dynamic, $expand_map, $hidden);
    }
    // Hide link if requested, but only if there is no dynamic tree data for it.
    elseif (isset($hidden[$path]) && !isset($tree_dynamic[$path])) {
      $tree[$key]['link']['access'] = FALSE;
      continue;
    }
    // Nothing to merge, if this parent path is not in our dynamic tree.
    if (!isset($tree_dynamic[$path])) {
      continue;
    }

    // Add expanded dynamic items.
    foreach ($tree_dynamic[$path] as $link) {
      // If the dynamic item has custom placeholder expansion parameters set,
      // use them, otherwise keep current.
      if (isset($link['expand_map'])) {
        // If there are currently no expansion parameters, we may use the new
        // set immediately.
        if (empty($expand_map)) {
          $current_expand_map = $link['expand_map'];
        }
        else {
          // Otherwise we need to filter out elements that differ from the
          // current set, i.e. that are not in the same path.
          $current_expand_map = array();
          foreach ($expand_map as $arguments) {
            foreach ($arguments as $placeholder => $value) {
              foreach ($link['expand_map'] as $new_arguments) {
                // Skip the new argument if it doesn't contain the current
                // replacement placeholders or if their values differ.
                if (!isset($new_arguments[$placeholder]) || $new_arguments[$placeholder] != $value) {
                  continue;
                }
                $current_expand_map[] = $new_arguments;
              }
            }
          }
        }
      }
      else {
        $current_expand_map = $expand_map;
      }

      // Skip dynamic items without expansion parameters.
      if (empty($current_expand_map)) {
        continue;
      }

      // Expand anonymous to named placeholders.
      // @see _menu_load_objects()
      $path_args = explode('/', $link['path']);
      $load_functions = unserialize($link['load_functions']);
      if (is_array($load_functions)) {
        foreach ($load_functions as $index => $function) {
          if ($function) {
            if (is_array($function)) {
              list($function,) = each($function);
            }
            // Add the loader function name minus "_load".
            $placeholder = '%' . substr($function, 0, -5);
            $path_args[$index] = $placeholder;
          }
        }
      }
      $path_dynamic = implode('/', $path_args);

      // Create new menu items using expansion arguments.
      foreach ($current_expand_map as $arguments) {
        // Create the cartesian product for all arguments and create new
        // menu items for each generated combination thereof.
        foreach (admin_menu_expand_args($arguments) as $replacements) {
          $newpath = strtr($path_dynamic, $replacements);
          // Skip this item, if any placeholder could not be replaced.
          // Faster than trying to invoke _menu_translate().
          if (strpos($newpath, '%') !== FALSE) {
            continue;
          }
          $map = explode('/', $newpath);
          $item = admin_menu_translate($link, $map);
          // Skip this item, if the current user does not have access.
          if (empty($item)) {
            continue;
          }
          // Build subtree using current replacement arguments.
          $new_expand_map = array();
          foreach ($replacements as $placeholder => $value) {
            $new_expand_map[$placeholder] = array($value);
          }
          admin_menu_merge_tree($item, $tree_dynamic, array($new_expand_map), $hidden);
          $tree[$key]['below'] += $item;
        }
      }
    }
    // Sort new subtree items.
    ksort($tree[$key]['below']);
  }
}

/**
 * Translate an expanded router item into a menu link suitable for rendering.
 *
 * @param $router_item
 *   A menu router item.
 * @param $map
 *   A path map with placeholders replaced.
 */
function admin_menu_translate($router_item, $map) {
  _menu_translate($router_item, $map, TRUE);

  // Run through hook_translated_menu_link_alter() to add devel information,
  // if configured.
  $router_item['menu_name'] = 'management';
  // @todo Invoke as usual like _menu_link_translate().
  admin_menu_translated_menu_link_alter($router_item, NULL);

  if ($router_item['access']) {
    // Override mlid to make this item unique; since these items are expanded
    // from dynamic items, the mlid is always the same, so each item would
    // replace any other.
    // @todo Doing this instead leads to plenty of duplicate links below
    //   admin/structure/menu; likely a hidden recursion problem.
    // $router_item['mlid'] = $router_item['href'] . $router_item['mlid'];
    $router_item['mlid'] = $router_item['href'];
    // Turn menu callbacks into regular menu items to make them visible.
    if ($router_item['type'] == MENU_CALLBACK) {
      $router_item['type'] = MENU_NORMAL_ITEM;
    }

    // @see _menu_tree_check_access()
    $key = (50000 + $router_item['weight']) . ' ' . $router_item['title'] . ' ' . $router_item['mlid'];
    return array($key => array(
      'link' => $router_item,
      'below' => array(),
    ));
  }

  return array();
}

/**
 * Create the cartesian product of multiple varying sized argument arrays.
 *
 * @param $arguments
 *   A two dimensional array of arguments.
 *
 * @see hook_admin_menu_map()
 */
function admin_menu_expand_args($arguments) {
  $replacements = array();

  // Initialize line cursors, move out array keys (placeholders) and assign
  // numeric keys instead.
  $i = 0;
  $placeholders = array();
  $new_arguments = array();
  foreach ($arguments as $placeholder => $values) {
    // Skip empty arguments.
    if (empty($values)) {
      continue;
    }
    $cursor[$i] = 0;
    $placeholders[$i] = $placeholder;
    $new_arguments[$i] = $values;
    $i++;
  }
  $arguments = $new_arguments;
  unset($new_arguments);

  if ($rows = count($arguments)) {
    do {
      // Collect current argument from each row.
      $row = array();
      for ($i = 0; $i < $rows; ++$i) {
        $row[$placeholders[$i]] = $arguments[$i][$cursor[$i]];
      }
      $replacements[] = $row;

      // Increment cursor position.
      $j = $rows - 1;
      $cursor[$j]++;
      while (!array_key_exists($cursor[$j], $arguments[$j])) {
        // No more arguments left: reset cursor, go to next line and increment
        // that cursor instead. Repeat until argument found or out of rows.
        $cursor[$j] = 0;
        if (--$j < 0) {
          // We're done.
          break 2;
        }
        $cursor[$j]++;
      }
    } while (1);
  }

  return $replacements;
}

/**
 * Build the administration menu as renderable menu links.
 *
 * @param $tree
 *   A data structure representing the administration menu tree as returned from
 *   menu_tree_all_data().
 *
 * @return
 *   The complete administration menu, suitable for theme_admin_menu_links().
 *
 * @see theme_admin_menu_links()
 * @see admin_menu_menu_alter()
 */
function admin_menu_links_menu($tree) {
  $links = array();
  foreach ($tree as $key => $data) {
    // Skip invisible items.
    if (!$data['link']['access'] || $data['link']['type'] == MENU_CALLBACK) {
      continue;
    }
    // Hide 'Administer' and make child links appear on this level.
    // @todo Make this configurable.
    if ($data['link']['router_path'] == 'admin') {
      if ($data['below']) {
        $links = array_merge($links, admin_menu_links_menu($data['below']));
      }
      continue;
    }
    // Omit alias lookups.
    $data['link']['localized_options']['alias'] = TRUE;
    // Remove description to prevent mouseover tooltip clashes.
    unset($data['link']['localized_options']['attributes']['title']);

    $links[$key] = array(
      '#title' => $data['link']['title'],
      '#href' => $data['link']['href'],
      '#options' => $data['link']['localized_options'],
      '#weight' => $data['link']['weight'],
    );
    if ($data['below']) {
      $links[$key] += admin_menu_links_menu($data['below']);
    }
  }
  return $links;
}

/**
 * Build icon menu links; mostly containing maintenance helpers.
 *
 * @see theme_admin_menu_links()
 */
function admin_menu_links_icon() {
  $destination = drupal_get_destination();

  $links = array(
    '#theme' => 'admin_menu_links',
    '#weight' => -100,
  );
  $links['icon'] = array(
    '#title' => theme('admin_menu_icon'),
    '#attributes' => array('class' => array('admin-menu-icon')),
    '#href' => '<front>',
    '#options' => array(
      'html' => TRUE,
    ),
  );
  // Add link to manually run cron.
  $links['icon']['cron'] = array(
    '#title' => t('Run cron'),
    '#weight' => 50,
    '#access' => user_access('administer site configuration'),
    '#href' => 'admin/reports/status/run-cron',
  );
  // Add link to run update.php.
  $links['icon']['update'] = array(
    '#title' => t('Run updates'),
    '#weight' => 50,
    '#access' => ($GLOBALS['user']->uid == 1 || !empty($GLOBALS['update_free_access'])),
    '#href' => base_path() . 'update.php',
    '#options' => array(
      'external' => TRUE,
    ),
  );
  // Add link to drupal.org.
  $links['icon']['drupal.org'] = array(
    '#title' => 'Drupal.org',
    '#weight' => 100,
    '#access' => user_access('display drupal links'),
    '#href' => 'http://drupal.org',
  );
  // Add links to project issue queues.
  foreach (module_list(FALSE, TRUE) as $module) {
    $info = drupal_parse_info_file(drupal_get_path('module', $module) . '/' . $module . '.info');
    if (!isset($info['project']) || isset($links['icon']['drupal.org'][$info['project']])) {
      continue;
    }
    $links['icon']['drupal.org'][$info['project']] = array(
      '#title' => t('@project issue queue', array('@project' => $info['name'])),
      '#weight' => ($info['project'] == 'drupal' ? -10 : 0),
      '#href' => 'http://drupal.org/project/issues/' . $info['project'],
      '#options' => array(
        'query' => array('version' => (isset($info['core']) ? $info['core'] : 'All')),
      ),
    );
  }
  // Add items to flush caches.
  $links['icon']['flush-cache'] = array(
    '#title' => t('Flush all caches'),
    '#weight' => 20,
    '#access' => user_access('flush caches'),
    '#href' => 'admin_menu/flush-cache',
    '#options' => array(
      'query' => $destination + array('token' => drupal_get_token('admin_menu/flush-cache')),
    ),
  );
  $caches = array(
    'admin_menu' => t('Administration menu'),
    'cache' => t('Cache tables'),
    'menu' => t('Menu'),
    'registry' => t('Class registry'),
    'requisites' => t('Page requisites'),
    'theme' => t('Theme registry'),
  );
  foreach ($caches as $arg => $title) {
    $links['icon']['flush-cache'][$arg] = array(
      '#title' => $title,
      '#href' => 'admin_menu/flush-cache/' . $arg,
      '#options' => array(
        'query' => $destination + array('token' => drupal_get_token('admin_menu/flush-cache/' . $arg)),
      ),
    );
  }
  // Add link to toggle developer modules (performance).
  $saved_state = variable_get('admin_menu_devel_modules_enabled', NULL);
  $links['icon']['toggle-modules'] = array(
    '#title' => isset($saved_state) ? t('Enable developer modules') : t('Disable developer modules'),
    '#weight' => 88,
    '#access' => user_access('administer site configuration'),
    '#href' => 'admin_menu/toggle-modules',
    '#options' => array(
      'query' => $destination + array('token' => drupal_get_token('admin_menu/toggle-modules')),
    ),
  );

  // Add Devel module links.
  if (module_exists('devel')) {
    // Add variable editor.
    $links['icon']['devel-variables'] = array(
      '#title' => t('Variable editor'),
      '#weight' => 20,
      '#access' => user_access('access devel information'),
      '#href' => 'devel/variable',
    );
  }

  return $links;
}

/**
 * Build user/action links; mostly account information and links.
 *
 * @see theme_admin_menu_links()
 */
function admin_menu_links_user() {
  $links = array(
    '#theme' => 'admin_menu_links',
    '#weight' => 100,
  );
  // Add link to show current authenticated/anonymous users.
  $links['user-counter'] = array(
    '#title' => admin_menu_get_user_count(),
    '#description' => t('Current anonymous / authenticated users'),
    '#weight' => -90,
    '#attributes' => array('class' => array('admin-menu-action', 'admin-menu-users')),
    '#href' => (user_access('administer users') ? 'admin/people/people' : 'user'),
  );
  $links['account'] = array(
    '#title' => $GLOBALS['user']->name,
    '#weight' => -99,
    '#attributes' => array('class' => array('admin-menu-action', 'admin-menu-account')),
    '#href' => 'user/' . $GLOBALS['user']->uid,
  );
  $links['logout'] = array(
    '#title' => t('Log out'),
    '#weight' => -100,
    '#attributes' => array('class' => array('admin-menu-action')),
    '#href' => 'user/logout',
  );

  // Add Devel module switch user links.
  $switch_links = module_invoke('devel', 'switch_user_list');
  if (!empty($switch_links) && count($switch_links) > 1) {
    foreach ($switch_links as $uid => $link) {
      $links['account'][$link['title']] = array(
        '#title' => $link['title'],
        '#description' => $link['attributes']['title'],
        '#href' => $link['href'],
        '#options' => array(
          'query' => $link['query'],
          'html' => !empty($link['html']),
        ),
      );
    }
  }

  return $links;
}

/**
 * Form builder function for module settings.
 */
function admin_menu_theme_settings() {
  $form['admin_menu_margin_top'] = array(
    '#type' => 'checkbox',
    '#title' => t('Adjust top margin'),
    '#default_value' => variable_get('admin_menu_margin_top', 1),
    '#description' => t('Shifts the site output down by approximately 20 pixels from the top of the viewport. If disabled, absolute- or fixed-positioned page elements may be covered by the administration menu.'),
  );
  $form['admin_menu_position_fixed'] = array(
    '#type' => 'checkbox',
    '#title' => t('Keep menu at top of page'),
    '#default_value' => variable_get('admin_menu_position_fixed', 0),
    '#description' => t('Displays the administration menu always at the top of the browser viewport (even when scrolling the page).'),
  );
  // @todo Re-confirm this with latest browser versions.
  $form['admin_menu_position_fixed']['#description'] .= '<br /><strong>' . t('In some browsers, this setting may result in a malformed page, an invisible cursor, non-selectable elements in forms, or other issues.') . '</strong>';

  $form['tweaks'] = array(
    '#type' => 'fieldset',
    '#title' => t('Advanced settings'),
  );
  $form['tweaks']['admin_menu_tweak_modules'] = array(
    '#type' => 'checkbox',
    '#title' => t('Collapse module groups on the <a href="!modules-url">%modules</a> page', array(
      '%modules' => t('Modules'),
      '!modules-url' => url('admin/modules'),
    )),
    '#default_value' => variable_get('admin_menu_tweak_modules', 0),
  );
  if (module_exists('util')) {
    $form['tweaks']['admin_menu_tweak_modules']['#description'] .= '<br /><strong>' . t('If the Utility module was installed for this purpose, it can be safely disabled and uninstalled.') . '</strong>';
  }
  $form['tweaks']['admin_menu_tweak_permissions'] = array(
    '#type' => 'checkbox',
    '#title' => t('Collapse module groups on the <a href="@permissions-url">%permissions</a> page', array(
      '%permissions' => t('Permissions'),
      '@permissions-url' => url('admin/people/permissions'),
    )),
    '#default_value' => variable_get('admin_menu_tweak_permissions', 0),
  );
  $form['tweaks']['admin_menu_tweak_tabs'] = array(
    '#type' => 'checkbox',
    '#title' => t('Move local tasks into menu'),
    '#default_value' => variable_get('admin_menu_tweak_tabs', 0),
    '#description' => t('Moves the tabs on all pages into the administration menu. Only possible for themes using the CSS classes <code>tabs primary</code> and <code>tabs secondary</code>.'),
  );

  // Fetch all available modules manually, since module_list() only returns
  // currently enabled modules, which makes this setting pointless if developer
  // modules are currently disabled.
  $all_modules = array();
  $result = db_query("SELECT name, filename, info FROM {system} WHERE type = 'module' ORDER BY name ASC");
  foreach ($result as $module) {
    if (file_exists($module->filename)) {
      $info = unserialize($module->info);
      $all_modules[$module->name] = $info['name'];
    }
  }
  $devel_modules = variable_get('admin_menu_devel_modules', _admin_menu_developer_modules());
  $devel_modules = array_intersect_key($all_modules, array_flip($devel_modules));
  $form['tweaks']['admin_menu_devel_modules_skip'] = array(
    '#type' => 'checkboxes',
    '#title' => t('Developer modules to keep enabled'),
    '#default_value' => variable_get('admin_menu_devel_modules_skip', array()),
    '#options' => $devel_modules,
    '#access' => !empty($devel_modules),
    '#description' => t('The selected modules will not be disabled when the link %disable-developer-modules below the icon in the menu is invoked.', array(
      '%disable-developer-modules' => t('Disable developer modules'),
    )),
  );

  return system_settings_form($form);
}

/**
 * Implementation of hook_form_FORM_ID_alter().
 *
 * Extends Devel module with Administration menu developer settings.
 */
function _admin_menu_form_devel_admin_settings_alter(&$form, $form_state) {
  // Shift system_settings_form buttons.
  $weight = isset($form['buttons']['#weight']) ? $form['buttons']['#weight'] : 0;
  $form['buttons']['#weight'] = $weight + 1;

  $form['admin_menu'] = array(
    '#type' => 'fieldset',
    '#title' => t('Administration menu settings'),
    '#collapsible' => TRUE,
    '#collapsed' => TRUE,
  );
  $display_options = array('mid', 'weight', 'pid');
  $display_options = array(0 => t('None'), 'mlid' => t('Menu link ID'), 'weight' => t('Weight'), 'plid' => t('Parent link ID'));
  $form['admin_menu']['admin_menu_display'] = array(
    '#type' => 'radios',
    '#title' => t('Display additional data for each menu item'),
    '#default_value' => variable_get('admin_menu_display', 0),
    '#options' => $display_options,
    '#description' => t('Display the selected items next to each menu item link.'),
  );
  $form['admin_menu']['admin_menu_show_all'] = array(
    '#type' => 'checkbox',
    '#title' => t('Display all menu items'),
    '#default_value' => variable_get('admin_menu_show_all', 0),
    '#description' => t('If enabled, all menu items are displayed regardless of your site permissions. <em>Note: Do not enable on a production site.</em>'),
  );
}

/**
 * Menu callback; Enable/disable developer modules.
 *
 * This can save up to 150ms on each uncached page request.
 */
function admin_menu_toggle_modules() {
  if (!isset($_GET['token']) || !drupal_valid_token($_GET['token'], current_path())) {
    return MENU_ACCESS_DENIED;
  }

  $rebuild = FALSE;
  $saved_state = variable_get('admin_menu_devel_modules_enabled', NULL);
  if (isset($saved_state)) {
    // Re-enable modules that were enabled before.
    module_enable($saved_state);
    variable_del('admin_menu_devel_modules_enabled');
    drupal_set_message(t('Enabled these modules: !module-list.', array('!module-list' => implode(', ', $saved_state))));
    $rebuild = TRUE;
  }
  else {
    // Allow site admins to override this variable via settings.php.
    $devel_modules = variable_get('admin_menu_devel_modules', _admin_menu_developer_modules());
    // Store currently enabled modules in a variable.
    $devel_modules = array_intersect(module_list(FALSE, FALSE), $devel_modules);
    $devel_modules = array_diff($devel_modules, variable_get('admin_menu_devel_modules_skip', array()));
    if (!empty($devel_modules)) {
      variable_set('admin_menu_devel_modules_enabled', $devel_modules);
      // Disable developer modules.
      module_disable($devel_modules);
      drupal_set_message(t('Disabled these modules: !module-list.', array('!module-list' => implode(', ', $devel_modules))));
      $rebuild = TRUE;
    }
    else {
      drupal_set_message(t('No developer modules are enabled.'));
    }
  }
  if ($rebuild) {
    // Make sure everything is rebuilt, basically a combination of the calls
    // from system_modules() and system_modules_submit().
    drupal_theme_rebuild();
    menu_rebuild();
    cache_clear_all('schema', 'cache');
    cache_clear_all();
    drupal_clear_css_cache();
    drupal_clear_js_cache();
    // Synchronize to catch any actions that were added or removed.
    actions_synchronize();
    // Finally, flush admin_menu's cache.
    admin_menu_flush_caches();
  }
  drupal_goto();
}

/**
 * Helper function to return a default list of developer modules.
 */
function _admin_menu_developer_modules() {
  return array(
    'admin_devel',
    'cache_disable',
    'coder',
    'content_copy',
    'debug',
    'delete_all',
    'demo',
    'devel',
    'devel_node_access',
    'devel_themer',
    'field_ui',
    'macro',
    'form_controller',
    'imagecache_ui',
    'journal',
    'rules_admin',
    'stringoverrides',
    'trace',
    'upgrade_status',
    'user_display_ui',
    'util',
    'views_ui',
    'views_theme_wizard',
  );
}

/**
 * Flush all caches or a specific one.
 *
 * @param $name
 *   (optional) Name of cache to flush.
 */
function admin_menu_flush_cache($name = NULL) {
  if (!isset($_GET['token']) || !drupal_valid_token($_GET['token'], current_path())) {
    return MENU_ACCESS_DENIED;
  }

  switch ($name) {
    case 'admin_menu':
      admin_menu_flush_caches();
      break;

    case 'menu':
      menu_rebuild();
      break;

    case 'registry':
      registry_rebuild();
      // Fall-through to clear cache tables, since registry information is
      // usually the base for other data that is cached (e.g. SimpleTests).

    case 'cache':
      // Don't clear cache_form - in-progress form submissions may break.
      // Ordered so clearing the page cache will always be the last action.
      // @see drupal_flush_all_caches()
      $core = array('cache', 'cache_bootstrap', 'cache_filter', 'cache_page');
      $cache_tables = array_merge(module_invoke_all('flush_caches'), $core);
      foreach ($cache_tables as $table) {
        cache_clear_all('*', $table, TRUE);
      }
      break;

    case 'requisites':
      // Change query-strings on css/js files to enforce reload for all users.
      _drupal_flush_css_js();

      drupal_clear_css_cache();
      drupal_clear_js_cache();
      break;

    case 'theme':
      system_rebuild_theme_data();
      drupal_theme_rebuild();
      break;

    default:
      // Flush all caches; no need to re-implement this.
      module_load_include('inc', 'system', 'system.admin');
      $form = $form_state = array();
      system_clear_cache_submit($form, $form_state);
      break;
  }
  drupal_goto();
}

/**
 * Render an icon to display in the administration menu.
 *
 * @ingroup themeable
 */
function theme_admin_menu_icon() {
  return '<img class="admin-menu-icon" src="' . (theme_get_setting('toggle_favicon') ? theme_get_setting('favicon') : base_path() . 'misc/favicon.ico') . '" width="16" height="16" alt="' . t('Home') . '" />';
}

